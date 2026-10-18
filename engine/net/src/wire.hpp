#pragma once

#include "bowl/core/types.hpp"

#include <span>
#include <string_view>
#include <vector>

// Reading and writing packets (little-endian), and the rendezvous protocol that net::Host and net::Rendezvous share.
namespace bowl::net::wire {

struct Writer {
    std::vector<u8> out;
    void u8_(u8 v) { out.push_back(v); }
    void u16_(u16 v) { u8_(static_cast<u8>(v)), u8_(static_cast<u8>(v >> 8)); }
    void u32_(u32 v) { u16_(static_cast<u16>(v)), u16_(static_cast<u16>(v >> 16)); }
    void u64_(u64 v) { u32_(static_cast<u32>(v)), u32_(static_cast<u32>(v >> 32)); }
    void bytes(std::span<const u8> b) { out.insert(out.end(), b.begin(), b.end()); }
    void text(std::string_view s) { // u8 length, bytes
        u8_(static_cast<u8>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
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
    std::string_view text() {
        auto b = bytes(u8_());
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    bool done() const { return at >= in.size(); }
};

// Rendezvous packets: u32 magic, u8 kind, then by kind:
//   HostRoom   u32 game protocol, room, then optionally u8 listed, u8 players, u8 max players, label
//                                               host to server, every second: "I take players for this room"
//   JoinRoom   u32 game protocol, room, u64 nonce, u8 route (0: auto, 1: relay)   player to server, until answered
//   Introduce  u64 nonce (0 to the host), u64 session, u8 1 + address, or u8 0   server to both: who the other is
//              (the player always hears; the host only for the auto route, and the relay route sends no address)
//   NoRoom     u64 nonce                        server to player: nobody hosts that room
//   Relay      u64 session, packet              both ways, through the server, when they can't reach each other
//   ListRooms  u32 game protocol, u64 nonce, u16 from, zeros up to list_answer_size
//                                               anyone to server: which listed rooms are open
//   RoomList   u64 nonce, u16 total, u8 count, then per room: room, label, u8 players, u8 max players
//   MatchRoom  u32 game protocol, u64 nonce, u8 route    player to server: join any listed room with space
//              (answered like JoinRoom: Introduce, or NoRoom when none has space)
// Rooms and labels are text up to 64 bytes; addresses are written by Address::write.
constexpr u32 rendezvous_magic = 0x5244564E;
enum RendezvousKind : u8 {
    HostRoom = 1, JoinRoom = 2, Introduce = 3, NoRoom = 4, Relay = 5, ListRooms = 6, RoomList = 7, MatchRoom = 8
};
constexpr usize max_room = 64;
// The largest RoomList answer, and so the size a ListRooms request is padded to: the server answers with no more bytes
// than it was sent, so it can't be used to flood an address someone forged.
constexpr usize list_answer_size = 1100;

} // namespace bowl::net::wire
