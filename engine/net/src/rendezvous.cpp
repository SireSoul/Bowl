#include "bowl/net/rendezvous.hpp"

#include "crypto.hpp"
#include "wire.hpp"

#include "bowl/core/time.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bowl::net {

using wire::Reader;
using wire::Writer;

struct Rendezvous::Impl {
    RendezvousSettings settings;
    Socket socket;
    struct Room {
        Address host;
        f64 seen = 0;
        bool listed = false; // offered to anyone who asks (ListRooms, MatchRoom)
        u8 players = 0, max_players = 0;
        std::string label;
        bool open() const { return listed && (max_players == 0 || players < max_players); }
    };
    std::map<std::pair<u32, std::string>, Room> rooms; // by game protocol and name
    struct Session {
        Address player, host;
        u64 nonce = 0;
        bool auto_route = true;
        f64 used = 0, refilled = 0;
        f64 to_host = 0, to_player = 0; // bytes the relay may still pass each way (refilled every second)
    };
    std::unordered_map<u64, Session> sessions;
    std::unordered_map<u64, u64> by_nonce; // a player's join nonce: its session, for when it asks again
    RendezvousStats stats;
    f64 now = 0, last_cleanup = 0;

    void introduce(u64 id, const Session& s) {
        Writer to_player;
        to_player.u32_(wire::rendezvous_magic);
        to_player.u8_(wire::Introduce);
        to_player.u64_(s.nonce);
        to_player.u64_(id);
        to_player.u8_(s.auto_route ? 1 : 0);
        if (s.auto_route) s.host.write(to_player.out);
        socket.send(s.player, to_player.out);
        if (!s.auto_route) return; // relayed only: neither side learns the other's address
        Writer to_host;
        to_host.u32_(wire::rendezvous_magic);
        to_host.u8_(wire::Introduce);
        to_host.u64_(0);
        to_host.u64_(id);
        to_host.u8_(1);
        s.player.write(to_host.out);
        socket.send(s.host, to_host.out);
    }

    void handle(const Address& from, std::span<const u8> packet) {
        Reader r{packet};
        if (r.u32_() != wire::rendezvous_magic) return;
        u8 kind = r.u8_();
        if (!r.ok) return;
        if (kind == wire::HostRoom) {
            u32 protocol = r.u32_();
            std::string_view name = r.text();
            if (!r.ok || name.empty() || name.size() > wire::max_room) return;
            Room room{from, now};
            if (!r.done()) { // a listing: older hosts don't send one
                room.listed = r.u8_() != 0;
                room.players = r.u8_();
                room.max_players = r.u8_();
                room.label = std::string(r.text().substr(0, wire::max_room));
                if (!r.ok) return;
            }
            auto key = std::make_pair(protocol, std::string(name));
            auto it = rooms.find(key);
            if (it == rooms.end()) {
                if (rooms.size() >= settings.max_rooms) return;
                rooms[key] = room;
            } else if (it->second.host == from || now - it->second.seen > settings.room_timeout) {
                it->second = room; // the same host again, or a new one for a room whose host went quiet
            } // else: someone else hosts it; the first host keeps it
            return;
        }
        if (kind == wire::ListRooms) {
            u32 protocol = r.u32_();
            u64 nonce = r.u64_();
            u16 from_index = r.u16_();
            if (!r.ok) return;
            std::vector<const std::pair<const std::pair<u32, std::string>, Room>*> open;
            for (const auto& entry : rooms)
                if (entry.first.first == protocol && entry.second.open() && now - entry.second.seen <= settings.room_timeout)
                    open.push_back(&entry);
            std::sort(open.begin(), open.end(), [](auto* a, auto* b) { // fullest first, then by name
                return a->second.players != b->second.players ? a->second.players > b->second.players
                                                              : a->first.second < b->first.second;
            });
            Writer w;
            w.u32_(wire::rendezvous_magic);
            w.u8_(wire::RoomList);
            w.u64_(nonce);
            w.u16_(static_cast<u16>(std::min<usize>(open.size(), 0xFFFF)));
            usize count_at = w.out.size();
            w.u8_(0);
            u8 count = 0;
            for (usize i = from_index; i < open.size() && count < 255; ++i) {
                const auto& [key, room] = *open[i];
                // One packet's worth, and never more than was asked with: a list sent to a forged address would make
                // this server a way to flood it (list_rooms pads its request to a full answer's size).
                usize limit = std::min(wire::list_answer_size, packet.size());
                if (w.out.size() + 2 + key.second.size() + room.label.size() + 2 > limit) break;
                w.text(key.second);
                w.text(room.label);
                w.u8_(room.players);
                w.u8_(room.max_players);
                ++count;
            }
            w.out[count_at] = count;
            socket.send(from, w.out);
            return;
        }
        if (kind == wire::JoinRoom || kind == wire::MatchRoom) {
            u32 protocol = r.u32_();
            std::string_view name = kind == wire::JoinRoom ? r.text() : std::string_view();
            u64 nonce = r.u64_();
            bool relay_only = r.u8_() != 0;
            if (!r.ok || nonce == 0) return;
            if (auto known = by_nonce.find(nonce); known != by_nonce.end()) { // asked again: the answer was lost
                auto s = sessions.find(known->second);
                if (s != sessions.end() && s->second.player == from) introduce(s->first, s->second);
                return;
            }
            Room* room = nullptr;
            if (kind == wire::JoinRoom) {
                auto it = rooms.find(std::make_pair(protocol, std::string(name)));
                if (it != rooms.end() && now - it->second.seen <= settings.room_timeout) room = &it->second;
            } else { // the fullest listed room that still has space
                for (auto& [key, candidate] : rooms)
                    if (key.first == protocol && candidate.open() && now - candidate.seen <= settings.room_timeout &&
                        (!room || candidate.players > room->players))
                        room = &candidate;
            }
            if (!room) {
                Writer w;
                w.u32_(wire::rendezvous_magic);
                w.u8_(wire::NoRoom);
                w.u64_(nonce);
                socket.send(from, w.out);
                return;
            }
            if (sessions.size() >= settings.max_sessions) return; // full: the player asks again later
            u64 id = 0;
            while (id == 0 || sessions.contains(id)) id = crypto::random_u64();
            Session s;
            s.player = from;
            s.host = room->host;
            s.nonce = nonce;
            s.auto_route = !relay_only;
            s.used = s.refilled = now;
            s.to_host = s.to_player = settings.relay_bytes_per_second;
            sessions[id] = s;
            by_nonce[nonce] = id;
            if (room->players < 255) ++room->players; // until the host's next word: several matches at once spread out
            ++stats.introductions;
            introduce(id, s);
            return;
        }
        if (kind == wire::Relay) {
            u64 id = r.u64_();
            auto it = sessions.find(id);
            if (!r.ok || it == sessions.end()) {
                ++stats.dropped_packets;
                return;
            }
            Session& s = it->second;
            bool from_player = from == s.player, from_host = from == s.host;
            if (!from_player && !from_host) {
                ++stats.dropped_packets;
                return;
            }
            if (now - s.refilled >= 1) { // a second's worth each way, refilled each second
                s.to_host = s.to_player = settings.relay_bytes_per_second;
                s.refilled = now;
            }
            f64& budget = from_player ? s.to_host : s.to_player;
            if (static_cast<f64>(packet.size()) > budget) {
                ++stats.dropped_packets;
                return;
            }
            budget -= static_cast<f64>(packet.size());
            s.used = now;
            ++stats.relayed_packets;
            stats.relayed_bytes += packet.size();
            socket.send(from_player ? s.host : s.player, packet); // the same Relay packet: the session says whose
        }
    }

    void cleanup() {
        for (auto it = rooms.begin(); it != rooms.end();)
            it = now - it->second.seen > settings.room_timeout ? rooms.erase(it) : std::next(it);
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (now - it->second.used > settings.session_timeout) {
                by_nonce.erase(it->second.nonce);
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        last_cleanup = now;
    }
};

Rendezvous::Rendezvous() : impl_(std::make_unique<Impl>()) {}
Rendezvous::~Rendezvous() = default;

bool Rendezvous::start(const RendezvousSettings& settings) {
    stop();
    impl_->settings = settings;
    return impl_->socket.open(settings.port, settings.ipv6);
}

void Rendezvous::stop() {
    impl_->socket.close();
    impl_->rooms.clear();
    impl_->sessions.clear();
    impl_->by_nonce.clear();
}

bool Rendezvous::running() const { return impl_->socket.is_open(); }
u16 Rendezvous::port() const { return impl_->socket.port(); }

void Rendezvous::update(f64 now) {
    if (!running()) return;
    Impl& m = *impl_;
    m.now = now >= 0 ? now : now_seconds();
    std::array<u8, 2048> buffer;
    Address from;
    for (int i = 0; i < 4096; ++i) {
        auto got = m.socket.receive(from, buffer);
        if (!got) break;
        m.handle(from, std::span<const u8>(buffer.data(), *got));
    }
    if (m.now - m.last_cleanup >= 1) m.cleanup();
}

void Rendezvous::handle(const Address& from, std::span<const u8> packet, f64 now) {
    Impl& m = *impl_;
    m.now = now;
    m.handle(from, packet);
    if (m.now - m.last_cleanup >= 1) m.cleanup();
}

RendezvousStats Rendezvous::stats() const {
    RendezvousStats s = impl_->stats;
    s.rooms = impl_->rooms.size();
    s.sessions = impl_->sessions.size();
    return s;
}

} // namespace bowl::net
