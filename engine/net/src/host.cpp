#include "bowl/net/host.hpp"

#include "crypto.hpp"
#include "wire.hpp"

#include "bowl/core/log.hpp"
#include "bowl/core/time.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace bowl::net {

// Packets, little-endian:
//   u32 protocol, u8 kind, then by kind:
//   ConnectRequest  u64 client salt, [32] client public key, [24] zeros (as long as the answer: no amplification)
//   ConnectAccept   u64 client salt, u64 server salt, [32] server public key, [16] tag over all of it (counter 0):
//                   proof that the server made the same keys, so it had the same secret
//   ConnectDeny     u64 client salt                             (the host is full)
//   Data            u64 token, u64 counter, [16] tag, then encrypted:
//                     u16 sequence, u16 ack, u32 ack bits, u8 acks valid, then messages:
//                     u8 flags (1: reliable, 2: more pieces follow), [u16 id if reliable], u16 length, bytes
//   Disconnect      u64 token, u64 counter, [16] tag (nothing encrypted)
//   Punch           nothing: a host sends it to a player it was introduced to, so its router lets the player in
// The token (client salt ^ server salt) names the connection. Each direction has its own key and counts its packets;
// a counter is never used twice, and one seen before (or 64 or more behind the newest) is turned away.

namespace {

enum Kind : u8 { ConnectRequest = 1, ConnectAccept = 2, ConnectDeny = 3, Data = 4, Disconnect = 5, Punch = 6 };
constexpr usize max_packet = 1200;
constexpr usize piece_size = 1024; // reliable messages are cut into pieces this big
constexpr u8 flag_reliable = 1, flag_more = 2;
constexpr usize window = 1024;     // packets remembered for acks
constexpr usize sealed_header = 4 + 1 + 8 + 8;                  // protocol, kind, token, counter
constexpr usize sealed_overhead = sealed_header + crypto::mac_size; // before the encrypted part
constexpr usize request_padding = 24;
constexpr f64 direct_wait = 2; // seconds an Auto join tries to reach the host directly before taking the relay

u64 nonzero_random() {
    for (;;)
        if (u64 v = crypto::random_u64()) return v;
}

// u16 numbers wrap: a is newer than b if it's ahead by less than half the range.
bool newer(u16 a, u16 b) { return a != b && static_cast<u16>(a - b) < 32768; }

using wire::Reader;
using wire::Writer;

} // namespace

struct Host::Delayed {
    Address to;
    std::vector<u8> packet;
    f64 send_at;
};

struct Host::Peer {
    // Joining: we asked a rendezvous server for a room's host. Connecting: we asked the host and wait for an answer.
    // Accepted: they asked and we answered, but they haven't yet sent a packet with the keys (with the wrong secret,
    // they never will). Connected: both ends have the keys.
    enum class State { Joining, Connecting, Accepted, Connected } state = State::Connecting;
    PeerId id = 0;
    Address address;      // where packets go: the peer, or the rendezvous server when relayed
    u64 relay_session = 0; // set: relayed, through `address`

    // join_room
    Address rendezvous;
    std::string room;
    Route route = Route::Auto;
    u64 join_nonce = 0, room_session = 0; // the rendezvous server's name for our introduction
    bool match = false;                    // quick_match: any listed room with space
    f64 direct_until = 0;
    u64 client_salt = 0, server_salt = 0;
    bool outgoing = false; // we connected to them
    crypto::KeyPair keys;                 // ours, made for this connection
    crypto::Key their_key{};              // their public key
    crypto::SessionKeys session;
    std::vector<u8> accept;               // the server's answer, sent again if the client asks again
    u64 send_counter = 1;                 // 0 is the answer's tag
    u64 receive_highest = 0, receive_seen = 0; // newest counter, and which of the 64 before it arrived
    f64 last_received = 0, last_sent = -1e9, started = 0;
    bool ack_owed = false;
    PeerStats stats;

    // Sending
    u16 next_sequence = 0;
    struct Sent {
        bool used = false, acked = false;
        u16 sequence = 0;
        f64 time = 0;
        std::vector<u16> reliable;
    };
    std::vector<Sent> sent = std::vector<Sent>(window);
    struct Outgoing {
        u16 id;
        u8 flags;
        std::vector<u8> data;
        f64 last_sent = -1e9;
        bool sent_once = false;
    };
    std::deque<Outgoing> reliable_out;
    u16 next_reliable = 0;
    std::vector<std::vector<u8>> unreliable_out;

    // Receiving
    bool any_received = false;
    u16 remote_sequence = 0;
    u32 remote_bits = 0;
    u16 next_expected = 0;
    std::map<u16, std::pair<u8, std::vector<u8>>> early; // reliable pieces ahead of the next expected one
    std::vector<u8> partial;                              // pieces of a message so far
    u32 acked_lately = 0, lost_lately = 0;
    bool any_acked = false;
    u16 highest_ack = 0;

    u64 token() const { return client_salt ^ server_salt; }
    const crypto::Key& sending_key() const { return outgoing ? session.client_to_server : session.server_to_client; }
    const crypto::Key& receiving_key() const { return outgoing ? session.server_to_client : session.client_to_server; }
    bool seen(u64 counter) const {
        if (counter == 0) return true;
        if (counter > receive_highest) return false;
        u64 back = receive_highest - counter;
        return back >= 64 || ((receive_seen >> back) & 1);
    }
    void mark_seen(u64 counter) {
        if (counter > receive_highest) {
            u64 shift = counter - receive_highest;
            receive_seen = shift >= 64 ? 0 : receive_seen << shift;
            receive_highest = counter;
        }
        receive_seen |= 1ull << (receive_highest - counter);
    }
    f32 resend_delay() const { return std::max(0.1f, stats.rtt * 1.25f); }
};

Host::Host() = default;
Host::~Host() { stop(); }

bool Host::start(const HostSettings& settings) {
    stop();
    settings_ = settings;
    random_ ^= static_cast<u64>(now_seconds() * 1e6) * 0x2545F4914F6CDD1Dull;
    return socket_.open(settings.port, settings.ipv6);
}

void Host::stop() {
    if (!socket_.is_open()) return;
    std::vector<PeerId> ids;
    for (auto& [id, p] : peers_) ids.push_back(id);
    for (PeerId id : ids) disconnect(id);
    for (auto& d : delayed_) socket_.send(d->to, d->packet); // flush what the simulated network held back
    delayed_.clear();
    socket_.close();
    peers_.clear();
}

bool Host::running() const { return socket_.is_open(); }
u16 Host::port() const { return socket_.port(); }

PeerId Host::connect(const Address& to) {
    if (!running()) return 0;
    auto p = std::make_unique<Peer>();
    p->id = next_id_++;
    p->address = to;
    p->outgoing = true;
    p->client_salt = nonzero_random();
    p->started = p->last_received = -1; // the next update's time: now_ is as old as the last update, maybe long ago
    PeerId id = p->id;
    peers_[id] = std::move(p);
    return id;
}

void Host::host_room(const Address& rendezvous, std::string_view room, const RoomInfo& info) {
    room_server_ = rendezvous;
    room_ = std::string(room.substr(0, wire::max_room));
    room_info_ = info;
    room_info_.label = info.label.substr(0, wire::max_room);
    room_announced_ = -1e9; // say so at the next update
}

void Host::list_rooms(const Address& rendezvous, u16 from) {
    if (!running()) return;
    list_server_ = rendezvous;
    list_nonce_ = nonzero_random();
    Writer w;
    w.u32_(wire::rendezvous_magic);
    w.u8_(wire::ListRooms);
    w.u32_(settings_.protocol);
    w.u64_(list_nonce_);
    w.u16_(from);
    socket_.send(rendezvous, w.out);
}

PeerId Host::quick_match(const Address& rendezvous, Route route) {
    PeerId id = join_room(rendezvous, "", route);
    if (id) peers_[id]->match = true;
    return id;
}

void Host::stop_hosting_room() { room_.clear(); }

PeerId Host::join_room(const Address& rendezvous, std::string_view room, Route route) {
    PeerId id = connect(rendezvous);
    if (id == 0) return 0;
    Peer& p = *peers_[id];
    p.state = Peer::State::Joining;
    p.rendezvous = rendezvous;
    p.room = std::string(room.substr(0, wire::max_room));
    p.route = route;
    p.join_nonce = nonzero_random();
    return id;
}

bool Host::is_rendezvous(const Address& address) const {
    if (!room_.empty() && address == room_server_) return true;
    if (list_nonce_ && address == list_server_) return true;
    for (const auto& [id, p] : peers_)
        if (p->join_nonce && p->rendezvous == address) return true;
    return false;
}

bool Host::send(PeerId id, std::span<const u8> data, Delivery delivery) {
    auto it = peers_.find(id);
    if (it == peers_.end() || it->second->state != Peer::State::Connected) return false;
    Peer& p = *it->second;
    if (delivery == Delivery::Unreliable) {
        if (data.size() > max_unreliable) return false;
        p.unreliable_out.emplace_back(data.begin(), data.end());
        return true;
    }
    if (data.size() > max_reliable) return false;
    usize at = 0;
    do { // cut into pieces; the reader puts them back together in order
        usize n = std::min(piece_size, data.size() - at);
        bool more = at + n < data.size();
        p.reliable_out.push_back({p.next_reliable++, static_cast<u8>(flag_reliable | (more ? flag_more : 0)),
                                  std::vector<u8>(data.begin() + static_cast<std::ptrdiff_t>(at),
                                                  data.begin() + static_cast<std::ptrdiff_t>(at + n))});
        at += n;
    } while (at < data.size());
    return true;
}

void Host::broadcast(std::span<const u8> data, Delivery delivery) {
    for (auto& [id, p] : peers_)
        if (p->state == Peer::State::Connected) send(id, data, delivery);
}

void Host::disconnect(PeerId id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    Peer& p = *it->second;
    if (p.state == Peer::State::Accepted || p.state == Peer::State::Connected) // the others have no keys to say it with
        for (int i = 0; i < 3; ++i) send_now(p.address, p.relay_session, sealed(p, Disconnect, {})); // it may be lost
    drop(id, false);
}

void Host::drop(PeerId id, bool notify) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    bool was_known = it->second->state == Peer::State::Connected || it->second->outgoing;
    peers_.erase(it);
    if (notify && was_known) events_.push_back({Event::Type::Disconnected, id, Delivery::Reliable, {}});
}

Host::Peer* Host::find(const Address& address, u64 session) {
    for (auto& [id, p] : peers_)
        if (p->state != Peer::State::Joining && p->address == address && p->relay_session == session) return p.get();
    return nullptr;
}

std::optional<Event> Host::receive() {
    if (events_.empty()) return std::nullopt;
    Event e = std::move(events_.front());
    events_.pop_front();
    return e;
}

std::vector<PeerId> Host::peers() const {
    std::vector<PeerId> out;
    for (const auto& [id, p] : peers_)
        if (p->state == Peer::State::Connected) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<PeerStats> Host::stats(PeerId id) const {
    auto it = peers_.find(id);
    if (it == peers_.end()) return std::nullopt;
    PeerStats s = it->second->stats;
    s.address = it->second->address;
    s.relayed = it->second->relay_session != 0;
    return s;
}

namespace {

std::vector<u8> wrapped_for_relay(u64 session, std::span<const u8> packet) {
    Writer w;
    w.u32_(wire::rendezvous_magic);
    w.u8_(wire::Relay);
    w.u64_(session);
    w.bytes(packet);
    return std::move(w.out);
}

} // namespace

void Host::send_now(const Address& to, u64 session, std::span<const u8> packet) {
    if (session) socket_.send(to, wrapped_for_relay(session, packet));
    else socket_.send(to, packet);
}

void Host::transmit(const Address& to, u64 session, std::vector<u8> packet, f64 now) {
    if (session) packet = wrapped_for_relay(session, packet);
    if (settings_.simulated_loss > 0 || settings_.simulated_latency > 0 || settings_.simulated_jitter > 0) {
        random_ ^= random_ << 13, random_ ^= random_ >> 7, random_ ^= random_ << 17;
        f64 r = static_cast<f64>(random_ % 1000000) / 1000000.0;
        if (r < settings_.simulated_loss) return;
        f64 jitter = settings_.simulated_jitter * static_cast<f64>((random_ >> 20) % 1000) / 1000.0;
        delayed_.push_back(std::make_unique<Delayed>(Delayed{to, std::move(packet), now + settings_.simulated_latency + jitter}));
        return;
    }
    socket_.send(to, packet);
}

std::vector<u8> Host::sealed(Peer& p, u8 kind, std::vector<u8> body) {
    Writer w;
    w.u32_(settings_.protocol);
    w.u8_(kind);
    w.u64_(p.token());
    u64 counter = p.send_counter++;
    w.u64_(counter);
    crypto::Mac mac;
    crypto::seal(body, mac, p.sending_key(), counter, w.out);
    w.bytes(mac);
    w.bytes(body);
    return std::move(w.out);
}

void Host::send_packet(Peer& p, f64 now, bool force) {
    if (p.state == Peer::State::Joining) { // asking the rendezvous server who hosts the room, until it says
        if (now - p.last_sent < 0.25) return;
        Writer w;
        w.u32_(wire::rendezvous_magic);
        w.u8_(p.match ? wire::MatchRoom : wire::JoinRoom);
        w.u32_(settings_.protocol);
        if (!p.match) w.text(p.room);
        w.u64_(p.join_nonce);
        w.u8_(p.route == Route::Relay ? 1 : 0);
        p.last_sent = now;
        socket_.send(p.rendezvous, w.out);
        return;
    }
    if (p.state == Peer::State::Connecting) { // a client asking; asked again until answered
        if (p.room_session && !p.relay_session && now >= p.direct_until) { // the host can't be reached: take the relay
            p.address = p.rendezvous;
            p.relay_session = p.room_session;
            p.last_sent = -1e9;
        }
        if (now - p.last_sent < 0.25) return;
        Writer w;
        w.u32_(settings_.protocol);
        w.u8_(ConnectRequest);
        w.u64_(p.client_salt);
        w.bytes(p.keys.public_key);
        w.out.resize(w.out.size() + request_padding);
        p.last_sent = now;
        transmit(p.address, p.relay_session, std::move(w.out), now);
        return;
    }
    if (p.state == Peer::State::Accepted) return; // nothing to send until they show they have the keys
    // A reliable piece goes out if it hasn't been acknowledged in a while, and only within `window` of the oldest
    // unacknowledged one: the receiver keeps no more than that ahead of what it has delivered.
    f32 delay = p.resend_delay();
    u16 oldest = p.reliable_out.empty() ? 0 : p.reliable_out.front().id;
    auto due = [&](const Peer::Outgoing& m) {
        return now - m.last_sent >= delay && static_cast<u16>(m.id - oldest) < window;
    };
    usize unreliable_at = 0;
    for (int packets = 0; packets < 16; ++packets) { // several a frame when there's a lot to send
        bool more = unreliable_at < p.unreliable_out.size() || std::any_of(p.reliable_out.begin(), p.reliable_out.end(), due);
        bool owed = packets == 0 && (force || p.ack_owed || now - p.last_sent >= settings_.keep_alive);
        if (!more && !owed) break;

        Writer w; // the part that gets encrypted
        u16 sequence = p.next_sequence++;
        w.u16_(sequence);
        w.u16_(p.remote_sequence);
        w.u32_(p.remote_bits);
        w.u8_(p.any_received ? 1 : 0);
        Peer::Sent& record = p.sent[sequence % window];
        if (record.used && !record.acked) ++p.lost_lately; // fell out of the window unacknowledged
        record = {true, false, sequence, now, {}};
        for (auto& m : p.reliable_out) {
            if (!due(m) || sealed_overhead + w.out.size() + m.data.size() + 5 > max_packet) continue;
            w.u8_(m.flags);
            w.u16_(m.id);
            w.u16_(static_cast<u16>(m.data.size()));
            w.bytes(m.data);
            if (m.sent_once) ++p.stats.resent;
            m.sent_once = true;
            m.last_sent = now;
            record.reliable.push_back(m.id);
        }
        for (; unreliable_at < p.unreliable_out.size(); ++unreliable_at) {
            const auto& m = p.unreliable_out[unreliable_at];
            if (sealed_overhead + w.out.size() + m.size() + 3 > max_packet) break; // the next packet takes it
            w.u8_(0);
            w.u16_(static_cast<u16>(m.size()));
            w.bytes(m);
        }
        p.ack_owed = false;
        p.last_sent = now;
        ++p.stats.sent;
        transmit(p.address, p.relay_session, sealed(p, Data, std::move(w.out)), now);
    }
    p.unreliable_out.clear(); // whatever didn't fit in 16 packets is dropped: it's unreliable
}

void Host::handle_rendezvous(const Address& from, std::span<const u8> packet, f64 now) {
    Reader r{packet};
    if (r.u32_() != wire::rendezvous_magic) return;
    u8 kind = r.u8_();
    if (!r.ok) return;
    if (kind == wire::Relay) {
        u64 session = r.u64_();
        if (r.ok && session) handle(from, session, packet.subspan(r.at), now);
        return;
    }
    u64 nonce = r.u64_();
    if (!r.ok) return;
    if (kind == wire::RoomList) { // an answer to list_rooms()
        if (nonce != list_nonce_ || !(from == list_server_)) return;
        u16 total = r.u16_();
        u8 count = r.u8_();
        std::vector<RoomListing> rooms;
        for (u8 i = 0; i < count && r.ok; ++i) {
            RoomListing listing;
            listing.room = std::string(r.text());
            listing.label = std::string(r.text());
            listing.players = r.u8_();
            listing.max_players = r.u8_();
            rooms.push_back(std::move(listing));
        }
        if (!r.ok) return;
        listed_ = std::move(rooms);
        listed_total_ = total;
        list_nonce_ = 0; // answered: anything more from it is old
        events_.push_back({Event::Type::RoomsListed, 0, Delivery::Reliable, {}});
        return;
    }
    if (kind == wire::Introduce && nonce == 0) { // to us as a room's host: a player is coming
        u64 session = r.u64_();
        bool has_address = r.u8_() != 0;
        std::optional<Address> player = has_address ? Address::read(packet, r.at) : std::nullopt;
        if (!r.ok || session == 0 || room_.empty() || !(from == room_server_) || !player) return;
        Writer punch; // opens our router to them, so their requests get through
        punch.u32_(settings_.protocol);
        punch.u8_(Punch);
        for (int i = 0; i < 2; ++i) transmit(*player, 0, punch.out, now);
        return;
    }
    Peer* p = nullptr;
    for (auto& [id, q] : peers_)
        if (q->state == Peer::State::Joining && q->join_nonce == nonce && q->rendezvous == from) p = q.get();
    if (!p) return;
    if (kind == wire::NoRoom) {
        drop(p->id, true);
        return;
    }
    if (kind != wire::Introduce) return;
    u64 session = r.u64_();
    bool has_address = r.u8_() != 0;
    std::optional<Address> host = has_address ? Address::read(packet, r.at) : std::nullopt;
    if (!r.ok || session == 0) return;
    p->room_session = session;
    p->state = Peer::State::Connecting;
    p->last_sent = -1e9; // ask the host right away
    p->last_received = now;
    if (p->route == Route::Relay || !host) {
        p->address = p->rendezvous;
        p->relay_session = session;
    } else {
        p->address = *host;
        p->direct_until = now + direct_wait;
    }
}

void Host::handle(const Address& from, u64 session, std::span<const u8> packet, f64 now) {
    Reader r{packet};
    if (r.u32_() != settings_.protocol) return;
    u8 kind = r.u8_();
    if (!r.ok) return;
    Peer* p = find(from, session);

    if (kind == ConnectRequest) {
        u64 client_salt = r.u64_();
        auto their_key = r.bytes(crypto::key_size);
        if (!r.ok || client_salt == 0) return;
        if (p && (p->outgoing || p->client_salt != client_salt ||
                  !std::equal(their_key.begin(), their_key.end(), p->their_key.begin())))
            return; // someone else from the same address: ignore
        if (!p) {
            usize connected = 0;
            for (auto& [id, q] : peers_) connected += q->outgoing ? 0 : 1;
            if (connected >= settings_.max_peers) {
                Writer deny;
                deny.u32_(settings_.protocol);
                deny.u8_(ConnectDeny);
                deny.u64_(client_salt);
                transmit(from, session, std::move(deny.out), now);
                return;
            }
            auto fresh = std::make_unique<Peer>();
            fresh->address = from;
            fresh->relay_session = session;
            fresh->client_salt = client_salt;
            fresh->server_salt = nonzero_random();
            std::copy(their_key.begin(), their_key.end(), fresh->their_key.begin());
            if (!crypto::derive(fresh->session, fresh->keys, fresh->their_key, false, client_salt, fresh->server_salt,
                                settings_.secret))
                return;
            Writer accept;
            accept.u32_(settings_.protocol);
            accept.u8_(ConnectAccept);
            accept.u64_(client_salt);
            accept.u64_(fresh->server_salt);
            accept.bytes(fresh->keys.public_key);
            crypto::Mac proof;
            crypto::seal({}, proof, fresh->session.server_to_client, 0, accept.out);
            accept.bytes(proof);
            fresh->accept = std::move(accept.out);
            fresh->id = next_id_++;
            fresh->state = Peer::State::Accepted;
            fresh->started = fresh->last_received = now;
            p = fresh.get();
            peers_[p->id] = std::move(fresh);
        }
        transmit(from, session, p->accept, now); // answered every time it's asked: the first may have been lost
        return;
    }
    if (!p) return;
    if (kind == ConnectAccept || kind == ConnectDeny) {
        u64 client_salt = r.u64_();
        if (!r.ok || !p->outgoing || client_salt != p->client_salt || p->state != Peer::State::Connecting) return;
        if (kind == ConnectDeny) {
            drop(p->id, true);
            return;
        }
        u64 server_salt = r.u64_();
        auto their_key = r.bytes(crypto::key_size);
        usize signed_part = r.at;
        auto proof_bytes = r.bytes(crypto::mac_size);
        if (!r.ok || server_salt == 0) return;
        crypto::Key key{};
        std::copy(their_key.begin(), their_key.end(), key.begin());
        crypto::SessionKeys agreed;
        crypto::Mac proof;
        std::copy(proof_bytes.begin(), proof_bytes.end(), proof.begin());
        // A different secret (or anyone tampering with the answer) makes different keys, and the proof fails.
        if (!crypto::derive(agreed, p->keys, key, true, client_salt, server_salt, settings_.secret) ||
            !crypto::open({}, proof, agreed.server_to_client, 0, packet.first(signed_part)))
            return;
        p->server_salt = server_salt;
        p->their_key = key;
        p->session = agreed;
        p->state = Peer::State::Connected;
        p->last_received = now;
        events_.push_back({Event::Type::Connected, p->id, Delivery::Reliable, {}});
        send_packet(*p, now, true); // tell the server right away that we heard it, with the keys
        return;
    }
    if (kind != Data && kind != Disconnect) return;
    handle_sealed(*p, kind, packet, r.at, now);
}

void Host::handle_sealed(Peer& peer, u8 kind, std::span<const u8> packet, usize at, f64 now) {
    Peer* p = &peer;
    Reader h{packet, at};
    u64 token = h.u64_(), counter = h.u64_();
    auto mac_bytes = h.bytes(crypto::mac_size);
    if (!h.ok || p->state == Peer::State::Connecting || token != p->token() || p->seen(counter)) return;
    std::vector<u8> body(packet.begin() + static_cast<std::ptrdiff_t>(h.at), packet.end());
    crypto::Mac mac;
    std::copy(mac_bytes.begin(), mac_bytes.end(), mac.begin());
    if (!crypto::open(body, mac, p->receiving_key(), counter, packet.first(sealed_header))) return;
    p->mark_seen(counter);
    if (p->state == Peer::State::Accepted) { // the first packet with the keys: now they're in
        p->state = Peer::State::Connected;
        events_.push_back({Event::Type::Connected, p->id, Delivery::Reliable, {}});
    }
    if (kind == Disconnect) {
        drop(p->id, true);
        return;
    }
    Reader r{body};
    u16 sequence = r.u16_(), ack = r.u16_();
    u32 ack_bits = r.u32_();
    bool has_acks = r.u8_() != 0; // nothing to acknowledge until they've heard from us
    if (!r.ok) return;
    p->last_received = now;
    ++p->stats.received;

    // What they've received of ours.
    auto acknowledge = [&](u16 s) {
        Peer::Sent& record = p->sent[s % window];
        if (!record.used || record.acked || record.sequence != s) return;
        record.acked = true;
        ++p->acked_lately;
        f32 sample = static_cast<f32>(now - record.time);
        p->stats.rtt = p->stats.rtt == 0 ? sample : p->stats.rtt + (sample - p->stats.rtt) * 0.1f;
        for (u16 id : record.reliable)
            std::erase_if(p->reliable_out, [&](const Peer::Outgoing& m) { return m.id == id; });
    };
    if (has_acks) acknowledge(ack);
    for (u16 i = 0; i < 32 && has_acks; ++i)
        if (ack_bits & (1u << i)) acknowledge(static_cast<u16>(ack - 1 - i));
    // Packets that slid out of the ack bits unacknowledged were lost.
    if (has_acks && (!p->any_acked || newer(ack, p->highest_ack))) {
        if (p->any_acked)
            for (u16 s = static_cast<u16>(p->highest_ack - 32), n = 0; s != static_cast<u16>(ack - 32) && n < window; ++s, ++n) {
                Peer::Sent& record = p->sent[s % window];
                if (record.used && !record.acked && record.sequence == s) record.acked = true, ++p->lost_lately;
            }
        p->any_acked = true;
        p->highest_ack = ack;
    }
    if (p->acked_lately + p->lost_lately >= 64) {
        p->stats.loss = static_cast<f32>(p->lost_lately) / static_cast<f32>(p->acked_lately + p->lost_lately);
        p->acked_lately = p->lost_lately = 0;
    }

    // What we've received of theirs, for our next acks. A packet seen before is acknowledged but not read again.
    bool fresh = true;
    if (!p->any_received) {
        p->any_received = true;
        p->remote_sequence = sequence;
        p->remote_bits = 0;
    } else if (newer(sequence, p->remote_sequence)) {
        u16 shift = static_cast<u16>(sequence - p->remote_sequence);
        p->remote_bits = shift >= 32 ? 0 : (p->remote_bits << shift) | (1u << (shift - 1));
        if (shift == 32) p->remote_bits = 1u << 31;
        p->remote_sequence = sequence;
    } else {
        u16 back = static_cast<u16>(p->remote_sequence - sequence);
        if (back == 0 || back > 32 || (p->remote_bits & (1u << (back - 1)))) fresh = false;
        else p->remote_bits |= 1u << (back - 1);
    }
    p->ack_owed = true;
    if (!fresh) return;

    // Messages.
    while (!r.done() && r.ok) {
        u8 flags = r.u8_();
        u16 id = (flags & flag_reliable) ? r.u16_() : 0;
        u16 length = r.u16_();
        auto bytes = r.bytes(length);
        if (!r.ok) break;
        if (!(flags & flag_reliable)) {
            events_.push_back({Event::Type::Message, p->id, Delivery::Unreliable, {bytes.begin(), bytes.end()}});
            continue;
        }
        if (id != p->next_expected && !newer(id, p->next_expected)) continue; // had it already
        if (static_cast<u16>(id - p->next_expected) > window) continue;      // too far ahead to keep
        p->early.try_emplace(id, flags, std::vector<u8>(bytes.begin(), bytes.end()));
    }
    // In order: deliver from the next expected one while the pieces are there.
    for (auto it = p->early.find(p->next_expected); it != p->early.end(); it = p->early.find(p->next_expected)) {
        auto [flags, bytes] = std::move(it->second);
        p->early.erase(it);
        ++p->next_expected;
        p->partial.insert(p->partial.end(), bytes.begin(), bytes.end());
        if (p->partial.size() > max_reliable) { // no Host sends one this big: a peer that does is misbehaving
            drop(p->id, true);
            return;
        }
        if (!(flags & flag_more)) {
            events_.push_back({Event::Type::Message, p->id, Delivery::Reliable, std::move(p->partial)});
            p->partial.clear();
        }
    }
}

void Host::update(f64 now) {
    if (!running()) return;
    now_ = now >= 0 ? now : now_seconds();
    // Held-back packets whose time has come (simulated network).
    for (auto it = delayed_.begin(); it != delayed_.end();) {
        if ((*it)->send_at <= now_) {
            socket_.send((*it)->to, (*it)->packet);
            it = delayed_.erase(it);
        } else {
            ++it;
        }
    }
    if (!room_.empty() && now_ - room_announced_ >= 1) { // still hosting the room: the server forgets quiet hosts
        Writer w;
        w.u32_(wire::rendezvous_magic);
        w.u8_(wire::HostRoom);
        w.u32_(settings_.protocol);
        w.text(room_);
        usize players = 1; // the host
        for (const auto& [id, p] : peers_) players += !p->outgoing && p->state == Peer::State::Connected;
        u32 max_players = room_info_.max_players ? room_info_.max_players : settings_.max_peers + 1;
        w.u8_(room_info_.listed ? 1 : 0);
        w.u8_(static_cast<u8>(std::min<usize>(players, 255)));
        w.u8_(static_cast<u8>(std::min<u32>(max_players, 255)));
        w.text(room_info_.label);
        socket_.send(room_server_, w.out);
        room_announced_ = now_;
    }
    std::array<u8, 2048> buffer;
    Address from;
    for (int i = 0; i < 4096; ++i) {
        auto got = socket_.receive(from, buffer);
        if (!got) break;
        std::span<const u8> packet(buffer.data(), *got);
        if (is_rendezvous(from)) handle_rendezvous(from, packet, now_);
        else if (!settings_.simulated_unreachable) handle(from, 0, packet, now_);
    }
    send_or_time_out(now_);
}

void Host::send_or_time_out(f64 now) {
    std::vector<PeerId> gone;
    for (auto& [id, p] : peers_) {
        if (p->last_received < 0) p->started = p->last_received = now; // connect()ed since the last update
        if (now - p->last_received > settings_.timeout) gone.push_back(id);
        else send_packet(*p, now, false);
    }
    std::sort(gone.begin(), gone.end());
    for (PeerId id : gone) drop(id, true);
}

} // namespace bowl::net
