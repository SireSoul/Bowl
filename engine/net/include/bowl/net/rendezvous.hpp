#pragma once

#include "bowl/net/socket.hpp"

#include <memory>
#include <span>

namespace bowl::net {

// The server players meet through when they can't reach each other directly: hosts behind home routers tell it which
// room they host (Host::host_room), players ask it for a room (Host::join_room), and it introduces them, so they can
// connect straight to each other. When their routers won't allow that, it relays their packets, which are encrypted
// end to end: it can't read or change them. Run it on a machine everyone can reach, with its port open, for example
// with the bowl-rendezvous tool. Like Host, it does everything inside update(), on the calling thread.
struct RendezvousSettings {
    u16 port = 27016;
    bool ipv6 = false;
    u32 max_rooms = 10000;
    u32 max_sessions = 10000;                // introductions remembered (each can be relayed)
    f32 room_timeout = 5;                    // a room goes when its host hasn't said it's still there for this long
    f32 session_timeout = 60;                // an introduction goes when nothing has been relayed for this long
    u32 relay_bytes_per_second = 256 * 1024; // per introduction, each way; more is dropped
};

struct RendezvousStats {
    usize rooms = 0, sessions = 0;
    u64 introductions = 0;
    u64 relayed_packets = 0, relayed_bytes = 0;
    u64 dropped_packets = 0; // over the relay limit, or not from either side of their introduction
};

class Rendezvous : NonCopyable {
public:
    Rendezvous();
    ~Rendezvous();

    bool start(const RendezvousSettings& settings = {});
    void stop();
    bool running() const;
    u16 port() const;

    // Receives, introduces and relays. Call it often (a loop with a short sleep is fine). `now` is in seconds; leave it
    // out to use the clock.
    void update(f64 now = -1);
    RendezvousStats stats() const;

private:
    friend struct FuzzAccess; // tests/fuzz hands packets straight to handle(), without a socket
    // One packet from `from`, as update() handles what its socket receives.
    void handle(const Address& from, std::span<const u8> packet, f64 now);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bowl::net
