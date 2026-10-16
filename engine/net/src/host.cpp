#include "bowl/net/host.hpp"

#include "bowl/core/log.hpp"
#include "bowl/core/time.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace bowl::net {

// Packets, little-endian:
//   u32 protocol, u8 kind, then by kind:
//   ConnectRequest  u64 client salt
//   ConnectAccept   u64 client salt, u64 server salt           (the connection's key is client ^ server)
//   ConnectDeny     u64 client salt                             (the host is full)
//   Data            u64 key, u16 sequence, u16 ack, u32 ack bits, u8 acks valid, then messages:
//                     u8 flags (1: reliable, 2: more pieces follow), [u16 id if reliable], u16 length, bytes
//   Disconnect      u64 key

namespace {

enum Kind : u8 { ConnectRequest = 1, ConnectAccept = 2, ConnectDeny = 3, Data = 4, Disconnect = 5 };
constexpr usize max_packet = 1200;
constexpr usize piece_size = 1024; // reliable messages are cut into pieces this big
constexpr u8 flag_reliable = 1, flag_more = 2;
constexpr usize window = 1024;     // packets remembered for acks

// u16 numbers wrap: a is newer than b if it's ahead by less than half the range.
bool newer(u16 a, u16 b) { return a != b && static_cast<u16>(a - b) < 32768; }

struct Writer {
    std::vector<u8> out;
    void u8_(u8 v) { out.push_back(v); }
    void u16_(u16 v) { u8_(static_cast<u8>(v)), u8_(static_cast<u8>(v >> 8)); }
    void u32_(u32 v) { u16_(static_cast<u16>(v)), u16_(static_cast<u16>(v >> 16)); }
    void u64_(u64 v) { u32_(static_cast<u32>(v)), u32_(static_cast<u32>(v >> 32)); }
    void bytes(std::span<const u8> b) { out.insert(out.end(), b.begin(), b.end()); }
};

struct Reader {
    std::span<const u8> in;
    usize at = 0;
    bool ok = true;
    bool need(usize n) {
        if (at + n > in.size()) ok = false;
        return ok;
    }
    u8 u8_() { return need(1) ? in[at++] : 0; }
    u16 u16_() {
        u16 lo = u8_();
        return static_cast<u16>(lo | (u8_() << 8));
    }
    u32 u32_() {
        u32 lo = u16_();
        return lo | (static_cast<u32>(u16_()) << 16);
    }
    u64 u64_() {
        u64 lo = u32_();
        return lo | (static_cast<u64>(u32_()) << 32);
    }
    std::span<const u8> bytes(usize n) {
        if (!need(n)) return {};
        auto s = in.subspan(at, n);
        at += n;
        return s;
    }
    bool done() const { return at >= in.size(); }
};

} // namespace

struct Host::Delayed {
    Address to;
    std::vector<u8> packet;
    f64 send_at;
};

struct Host::Peer {
    enum class State { Connecting, Connected } state = State::Connecting;
    PeerId id = 0;
    Address address;
    u64 client_salt = 0, server_salt = 0;
    bool outgoing = false; // we connected to them
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

    u64 key() const { return client_salt ^ server_salt; }
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
    random_ ^= random_ << 13, random_ ^= random_ >> 7, random_ ^= random_ << 17;
    p->client_salt = random_ | 1;
    p->started = p->last_received = now_;
    PeerId id = p->id;
    peers_[id] = std::move(p);
    return id;
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
    Writer w;
    w.u32_(settings_.protocol);
    w.u8_(Disconnect);
    w.u64_(it->second->key());
    for (int i = 0; i < 3; ++i) socket_.send(it->second->address, w.out); // said a few times: it may be lost
    drop(id, false);
}

void Host::drop(PeerId id, bool notify) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    bool was_known = it->second->state == Peer::State::Connected || it->second->outgoing;
    peers_.erase(it);
    if (notify && was_known) events_.push_back({Event::Type::Disconnected, id, Delivery::Reliable, {}});
}

Host::Peer* Host::find(const Address& address) {
    for (auto& [id, p] : peers_)
        if (p->address == address) return p.get();
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
    return s;
}

void Host::transmit(const Address& to, std::vector<u8> packet, f64 now) {
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

void Host::send_packet(Peer& p, f64 now, bool force) {
    if (p.state == Peer::State::Connecting) { // a client asking; asked again until answered
        if (now - p.last_sent < 0.25) return;
        Writer w;
        w.u32_(settings_.protocol);
        w.u8_(ConnectRequest);
        w.u64_(p.client_salt);
        p.last_sent = now;
        transmit(p.address, std::move(w.out), now);
        return;
    }
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

        Writer w;
        w.u32_(settings_.protocol);
        w.u8_(Data);
        w.u64_(p.key());
        u16 sequence = p.next_sequence++;
        w.u16_(sequence);
        w.u16_(p.remote_sequence);
        w.u32_(p.remote_bits);
        w.u8_(p.any_received ? 1 : 0);
        Peer::Sent& record = p.sent[sequence % window];
        if (record.used && !record.acked) ++p.lost_lately; // fell out of the window unacknowledged
        record = {true, false, sequence, now, {}};
        for (auto& m : p.reliable_out) {
            if (!due(m) || w.out.size() + m.data.size() + 5 > max_packet) continue;
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
            if (w.out.size() + m.size() + 3 > max_packet) break; // the next packet takes it
            w.u8_(0);
            w.u16_(static_cast<u16>(m.size()));
            w.bytes(m);
        }
        p.ack_owed = false;
        p.last_sent = now;
        ++p.stats.sent;
        transmit(p.address, std::move(w.out), now);
    }
    p.unreliable_out.clear(); // whatever didn't fit in 16 packets is dropped: it's unreliable
}

void Host::handle(const Address& from, std::span<const u8> packet, f64 now) {
    Reader r{packet};
    if (r.u32_() != settings_.protocol) return;
    u8 kind = r.u8_();
    if (!r.ok) return;
    Peer* p = find(from);

    if (kind == ConnectRequest) {
        u64 client_salt = r.u64_();
        if (!r.ok) return;
        if (p && p->client_salt != client_salt) return; // someone else from the same address: ignore
        if (!p) {
            usize connected = 0;
            for (auto& [id, q] : peers_) connected += q->outgoing ? 0 : 1;
            if (connected >= settings_.max_peers) {
                Writer deny;
                deny.u32_(settings_.protocol);
                deny.u8_(ConnectDeny);
                deny.u64_(client_salt);
                transmit(from, std::move(deny.out), now);
                return;
            }
            auto fresh = std::make_unique<Peer>();
            fresh->id = next_id_++;
            fresh->address = from;
            fresh->client_salt = client_salt;
            random_ ^= random_ << 13, random_ ^= random_ >> 7, random_ ^= random_ << 17;
            fresh->server_salt = random_ | 2;
            fresh->state = Peer::State::Connected;
            fresh->started = fresh->last_received = now;
            p = fresh.get();
            peers_[p->id] = std::move(fresh);
            events_.push_back({Event::Type::Connected, p->id, Delivery::Reliable, {}});
        }
        Writer accept; // answered every time it's asked: the first answer may have been lost
        accept.u32_(settings_.protocol);
        accept.u8_(ConnectAccept);
        accept.u64_(p->client_salt);
        accept.u64_(p->server_salt);
        transmit(from, std::move(accept.out), now);
        return;
    }
    if (!p) return;
    if (kind == ConnectAccept || kind == ConnectDeny) {
        u64 client_salt = r.u64_();
        u64 server_salt = kind == ConnectAccept ? r.u64_() : 0;
        if (!r.ok || !p->outgoing || client_salt != p->client_salt) return;
        if (kind == ConnectDeny) {
            drop(p->id, true);
            return;
        }
        if (p->state == Peer::State::Connecting) {
            p->server_salt = server_salt;
            p->state = Peer::State::Connected;
            p->last_received = now;
            events_.push_back({Event::Type::Connected, p->id, Delivery::Reliable, {}});
            send_packet(*p, now, true); // tell the server right away that we heard it
        }
        return;
    }
    u64 key = r.u64_();
    if (!r.ok || key != p->key() || p->state != Peer::State::Connected) return;
    if (kind == Disconnect) {
        drop(p->id, true);
        return;
    }
    if (kind != Data) return;
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
    std::array<u8, 2048> buffer;
    Address from;
    for (int i = 0; i < 4096; ++i) {
        auto got = socket_.receive(from, buffer);
        if (!got) break;
        handle(from, std::span(buffer.data(), *got), now_);
    }
    std::vector<PeerId> gone;
    for (auto& [id, p] : peers_) {
        if (now_ - p->last_received > settings_.timeout) gone.push_back(id);
        else send_packet(*p, now_, false);
    }
    std::sort(gone.begin(), gone.end());
    for (PeerId id : gone) drop(id, true);
}

} // namespace bowl::net
