#pragma once

#include "bowl/net/socket.hpp"

#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace bowl::net {

// Connections over UDP: the same Host is a server (others connect to it) or a client (it connects to one), or both.
// Messages are either reliable (delivered once, in order, resent until acknowledged; any size up to 64 KB) or
// unreliable (sent once, may be lost or arrive out of order; up to 1 KB). Everything happens inside update(), on the
// calling thread; there are no threads of its own.
//
// Every connection is encrypted: the two ends agree on fresh keys when they connect (X25519), and every packet after
// that is encrypted and authenticated (XChaCha20-Poly1305), so nobody on the way can read, change, replay or forge
// one, or disconnect a peer. Without a `secret`, someone who can intercept and rewrite the handshake itself could sit
// in the middle; with one (the same on both ends: a password for a private game, or a value built into the game),
// they can't, and a host only lets in peers that know it.
//
//     net::Host host;
//     host.start({.port = 27015});                       // a server; clients start with no port
//     net::PeerId server = client.connect(*net::Address::resolve("127.0.0.1", 27015));
//
//     // every frame
//     host.update();
//     while (auto e = host.receive()) { ... e->type, e->peer, e->data ... }
//     host.send(peer, bytes);                            // reliable by default
using PeerId = u32; // 0: none

enum class Delivery : u8 { Unreliable, Reliable };

// How join_room reaches the host. Auto: straight to it when the routers in between let it (most home routers do,
// once both sides have sent to each other), and through the rendezvous server's relay when they don't. Relay: always
// through the relay, so the players never learn each other's addresses.
enum class Route : u8 { Auto, Relay };

struct HostSettings {
    u16 port = 0;              // 0: any free port (what clients want)
    bool ipv6 = false;         // an IPv6 socket also talks to IPv4
    u32 max_peers = 32;        // how many may connect to this host
    u32 protocol = 0x424F574C; // hosts with different numbers ignore each other: put your game's version in it
    std::string secret;        // optional; both ends need the same one to connect (see above)
    f32 timeout = 8;           // seconds without hearing from a peer before it's gone
    f32 keep_alive = 0.25f;    // seconds between packets when there's nothing to say
    // For testing on one machine: lose this share of outgoing packets, and hold them back this long (plus up to
    // `jitter`), like a real network would.
    f32 simulated_loss = 0;
    f32 simulated_latency = 0;
    f32 simulated_jitter = 0;
    // For testing the relay: as if behind a router nothing gets through, only packets from rendezvous servers arrive.
    bool simulated_unreachable = false;
};

struct Event {
    enum class Type : u8 { Connected, Disconnected, Message };
    Type type = Type::Message;
    PeerId peer = 0;
    Delivery delivery = Delivery::Reliable;
    std::vector<u8> data; // Message only
};

struct PeerStats {
    Address address;
    f32 rtt = 0;   // round trip, seconds (smoothed)
    f32 loss = 0;  // share of packets lost lately, 0..1
    u64 sent = 0, received = 0, resent = 0; // packets, packets, reliable messages sent again
    bool relayed = false;                   // through a rendezvous server's relay (join_room)
};

class Host : NonCopyable {
public:
    static constexpr usize max_unreliable = 1024;
    static constexpr usize max_reliable = 64 * 1024;

    Host();
    ~Host();

    bool start(const HostSettings& settings = {});
    // Tells every peer goodbye and closes the socket.
    void stop();
    bool running() const;
    u16 port() const;

    // Starts connecting; the peer's Connected (or, if nobody answers in time, Disconnected) event follows. A host
    // with a different `secret` never answers, so that ends in Disconnected too.
    PeerId connect(const Address& to);

    // Rooms, through a rendezvous server (net::Rendezvous; the bowl-rendezvous tool runs one on a machine everyone can
    // reach), for players behind home routers that can't take connections. A host names a room and keeps telling the
    // server it's there; a player joins by name, the server introduces them, and they connect: directly if they can,
    // through the server's relay if not. The connection is the same as connect()'s, encrypted end to end, so the
    // server only ever passes on packets it can't read. Room names are up to 64 bytes; make them hard to guess (a
    // random code) unless the room is meant to be public.
    void host_room(const Address& rendezvous, std::string_view room);
    void stop_hosting_room();
    // Like connect(): Connected follows, or Disconnected if nobody hosts the room or nobody answers in time.
    PeerId join_room(const Address& rendezvous, std::string_view room, Route route = Route::Auto);
    // False if the peer isn't connected or the message is too big for its delivery.
    bool send(PeerId peer, std::span<const u8> data, Delivery delivery = Delivery::Reliable);
    void broadcast(std::span<const u8> data, Delivery delivery = Delivery::Reliable);
    void disconnect(PeerId peer);

    // Receives and sends. Call once a frame or more. `now` is in seconds; leave it out to use the clock.
    void update(f64 now = -1);
    // The next thing that happened, or nothing.
    std::optional<Event> receive();

    std::vector<PeerId> peers() const; // connected ones
    std::optional<PeerStats> stats(PeerId peer) const;

private:
    friend struct FuzzAccess; // tests/fuzz hands packets straight to the handlers below
    struct Peer;
    struct Delayed;
    void handle(const Address& from, u64 session, std::span<const u8> packet, f64 now);
    void handle_sealed(Peer& peer, u8 kind, std::span<const u8> packet, usize header, f64 now);
    void handle_rendezvous(const Address& from, std::span<const u8> packet, f64 now);
    bool is_rendezvous(const Address& address) const;
    void send_packet(Peer& peer, f64 now, bool force);
    // The end of update(): peers not heard from in a while are gone, the others get what's waiting for them.
    void send_or_time_out(f64 now);
    std::vector<u8> sealed(Peer& peer, u8 kind, std::vector<u8> body);
    // `session` set: the packet goes through the relay at `to`, wrapped for it.
    void transmit(const Address& to, u64 session, std::vector<u8> packet, f64 now);
    void send_now(const Address& to, u64 session, std::span<const u8> packet);
    void drop(PeerId id, bool notify);
    Peer* find(const Address& address, u64 session);

    HostSettings settings_;
    Socket socket_;
    std::unordered_map<PeerId, std::unique_ptr<Peer>> peers_;
    std::deque<Event> events_;
    std::vector<std::unique_ptr<Delayed>> delayed_;
    PeerId next_id_ = 1;
    Address room_server_; // host_room
    std::string room_;
    f64 room_announced_ = -1e9;
    u64 random_ = 0x9E3779B97F4A7C15ull;
    f64 now_ = 0;
};

// Bytes for plain structs (trivially copyable inputs, small state), for send() and lockstep/rollback inputs.
template <class T> std::vector<u8> to_bytes(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "to_bytes is for plain structs");
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}
template <class T> std::optional<T> from_bytes(std::span<const u8> bytes) {
    static_assert(std::is_trivially_copyable_v<T>, "from_bytes is for plain structs");
    if (bytes.size() != sizeof(T)) return std::nullopt;
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

} // namespace bowl::net
