# net

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/net/host.hpp](#bowlnethosthpp)
- [bowl/net/lockstep.hpp](#bowlnetlockstephpp)
- [bowl/net/rendezvous.hpp](#bowlnetrendezvoushpp)
- [bowl/net/replication.hpp](#bowlnetreplicationhpp)
- [bowl/net/rollback.hpp](#bowlnetrollbackhpp)
- [bowl/net/socket.hpp](#bowlnetsockethpp)

## bowl/net/host.hpp

`#include "bowl/net/host.hpp"` · namespace `bowl::net`

Connections over UDP: the same Host is a server (others connect to it) or a client (it connects to one), or both.
Messages are either reliable (delivered once, in order, resent until acknowledged; any size up to 64 KB) or
unreliable (sent once, may be lost or arrive out of order; up to 1 KB). Everything happens inside update(), on the
calling thread; there are no threads of its own.

Every connection is encrypted: the two ends agree on fresh keys when they connect (X25519), and every packet after
that is encrypted and authenticated (XChaCha20-Poly1305), so nobody on the way can read, change, replay or forge
one, or disconnect a peer. Without a `secret`, someone who can intercept and rewrite the handshake itself could sit
in the middle; with one (the same on both ends: a password for a private game, or a value built into the game),
they can't, and a host only lets in peers that know it.

```cpp
net::Host host;
host.start({.port = 27015});                       // a server; clients start with no port
net::PeerId server = client.connect(*net::Address::resolve("127.0.0.1", 27015));

// every frame
host.update();
while (auto e = host.receive()) { ... e->type, e->peer, e->data ... }
host.send(peer, bytes);                            // reliable by default
```

```cpp
using PeerId = u32; // 0: none
```

```cpp
enum class Delivery : u8 { Unreliable, Reliable };
```

How join_room reaches the host. Auto: straight to it when the routers in between let it (most home routers do,
once both sides have sent to each other), and through the rendezvous server's relay when they don't. Relay: always
through the relay, so the players never learn each other's addresses.

```cpp
enum class Route : u8 { Auto, Relay };
```

### HostSettings

```cpp
struct HostSettings {
    u16 port = 0;              // 0: any free port (what clients want)
    bool ipv6 = false;         // an IPv6 socket also talks to IPv4
    bool local_only = false;   // listen on this computer's loopback only: nothing on a network can reach it
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
```

### RoomInfo

How a hosted room shows in the rendezvous server's list (host_room). Unlisted rooms are only found by name.

```cpp
struct RoomInfo {
    bool listed = false;   // anyone can see it with list_rooms() and land in it with quick_match()
    std::string label;     // what the list shows: the host's name, the game mode... (up to 64 bytes)
    u8 max_players = 0;    // counting the host; 0: max_peers + 1
};
```

### RoomListing

A listed room, from list_rooms().

```cpp
struct RoomListing {
    std::string room, label;
    u8 players = 0, max_players = 0;
};
```

### Event

```cpp
struct Event {
    // RoomsListed: an answer to list_rooms() arrived (peer 0); listed_rooms() has it.
    enum class Type : u8 { Connected, Disconnected, Message, RoomsListed };
    Type type = Type::Message;
    PeerId peer = 0;
    Delivery delivery = Delivery::Reliable;
    std::vector<u8> data; // Message only
};
```

### PeerStats

```cpp
struct PeerStats {
    Address address;
    f32 rtt = 0;   // round trip, seconds (smoothed)
    f32 loss = 0;  // share of packets lost lately, 0..1
    u64 sent = 0, received = 0, resent = 0; // packets, packets, reliable messages sent again
    bool relayed = false;                   // through a rendezvous server's relay (join_room)
};
```

### Host

```cpp
class Host : NonCopyable
```

```cpp
static constexpr usize max_unreliable = 1024;
static constexpr usize max_reliable = 64 * 1024;
```

```cpp
Host();
~Host();
```

```cpp
bool start(const HostSettings& settings = {});
```

Tells every peer goodbye and closes the socket.

```cpp
void stop();
bool running() const;
u16 port() const;
```

Starts connecting; the peer's Connected (or, if nobody answers in time, Disconnected) event follows. A host
with a different `secret` never answers, so that ends in Disconnected too.

```cpp
PeerId connect(const Address& to);
```

Rooms, through a rendezvous server (net::Rendezvous; the bowl-rendezvous tool runs one on a machine everyone can
reach), for players behind home routers that can't take connections. A host names a room and keeps telling the
server it's there; a player joins by name, the server introduces them, and they connect: directly if they can,
through the server's relay if not. The connection is the same as connect()'s, encrypted end to end, so the
server only ever passes on packets it can't read. Room names are up to 64 bytes; make them hard to guess (a
random code) unless the room is meant to be public.

```cpp
void host_room(const Address& rendezvous, std::string_view room, const RoomInfo& info = {});
void stop_hosting_room();
```

Like connect(): Connected follows, or Disconnected if nobody hosts the room or nobody answers in time.

```cpp
PeerId join_room(const Address& rendezvous, std::string_view room, Route route = Route::Auto);
```

Lobbies. Listed rooms (RoomInfo::listed) of this game (the same `protocol`) with space, fullest first:
list_rooms() asks for them (a RoomsListed event follows, then listed_rooms() has up to a packet's worth, from
`from` on, and listed_total() says how many there are); quick_match() joins the fullest one that has space, like
join_room, or ends in Disconnected when none has. The host's player count reaches the server once a second.

```cpp
void list_rooms(const Address& rendezvous, u16 from = 0);
const std::vector<RoomListing>& listed_rooms() const;
u16 listed_total() const;
PeerId quick_match(const Address& rendezvous, Route route = Route::Auto);
```

False if the peer isn't connected or the message is too big for its delivery.

```cpp
bool send(PeerId peer, std::span<const u8> data, Delivery delivery = Delivery::Reliable);
void broadcast(std::span<const u8> data, Delivery delivery = Delivery::Reliable);
void disconnect(PeerId peer);
```

Receives and sends. Call once a frame or more. `now` is in seconds; leave it out to use the clock.

```cpp
void update(f64 now = -1);
```

The next thing that happened, or nothing.

```cpp
std::optional<Event> receive();
```

```cpp
std::vector<PeerId> peers() const; // connected ones
std::optional<PeerStats> stats(PeerId peer) const;
```

Bytes for plain structs (trivially copyable inputs, small state), for send() and lockstep/rollback inputs.

```cpp
template <class T> std::vector<u8> to_bytes(const T& value);
template <class T> std::optional<T> from_bytes(std::span<const u8> bytes);
```

## bowl/net/lockstep.hpp

`#include "bowl/net/lockstep.hpp"` · namespace `bowl::net`

### InputMessage

One player's inputs for a run of ticks, as Lockstep and Rollback send them. Inputs are plain structs (trivially
copyable, the same layout on every machine) with an ==.

```cpp
template <class Input> struct InputMessage
```

```cpp
static constexpr u32 magic = 0x54504E49; // "INPT"
```

```cpp
u32 player = 0;
u32 first_tick = 0;
std::vector<Input> inputs;
```

```cpp
std::vector<u8> encode() const;
static std::optional<InputMessage> decode(std::span<const u8> bytes);
```

### Lockstep

Lockstep: every machine runs the same simulation from the same inputs, and a tick runs only once every player's
input for it is in. Inputs are taken `delay` ticks ahead so they have time to arrive; with a slow player everyone
waits. Send the messages reliably (Delivery::Reliable): each one goes once.

```cpp
net::Lockstep<Input> lockstep(players, my_index);
// every frame
if (auto m = lockstep.local(read_input())) host.broadcast(*m);
while (auto e = host.receive()) if (e->type == net::Event::Type::Message) lockstep.receive(e->data);
if (lockstep.ready()) {
    simulate(lockstep.inputs()); // one per player, in player order
    lockstep.advance();
}
```

```cpp
template <class Input> class Lockstep
```

```cpp
Lockstep(u32 players, u32 local, u32 delay = 2);
```

```cpp
u32 tick() const;
u32 delay() const;
```

This player's input for tick() + delay. Call every frame; the first call for a tick takes it and returns the
message to send everyone else, later ones return nothing.

```cpp
std::optional<std::vector<u8>> local(const Input& input);
```

Another player's message. False if it isn't one.

```cpp
bool receive(std::span<const u8> bytes);
```

Whether every input for tick() is in.

```cpp
bool ready() const;
```

The inputs for tick(), one per player. Only while ready().

```cpp
std::span<const Input> inputs() const;
void advance();
```

Players whose input for tick() hasn't come yet.

```cpp
std::vector<u32> waiting_for() const;
```

## bowl/net/rendezvous.hpp

`#include "bowl/net/rendezvous.hpp"` · namespace `bowl::net`

### RendezvousSettings

The server players meet through when they can't reach each other directly: hosts behind home routers tell it which
room they host (Host::host_room), players ask it for a room (Host::join_room), and it introduces them, so they can
connect straight to each other. When their routers won't allow that, it relays their packets, which are encrypted
end to end: it can't read or change them. Run it on a machine everyone can reach, with its port open, for example
with the bowl-rendezvous tool. Like Host, it does everything inside update(), on the calling thread.

```cpp
struct RendezvousSettings {
    u16 port = 27016;
    bool ipv6 = false;
    u32 max_rooms = 10000;
    u32 max_sessions = 10000;                // introductions remembered (each can be relayed)
    f32 room_timeout = 5;                    // a room goes when its host hasn't said it's still there for this long
    f32 session_timeout = 60;                // an introduction goes when nothing has been relayed for this long
    // Introductions nothing has been relayed through yet: these go sooner, at most this many wait at once for one
    // address (players behind one router share it), and when every slot is taken the oldest of them makes way. So
    // join requests from forged addresses can't keep real players out for long.
    f32 join_timeout = 10;
    u32 max_joining_per_address = 32;
    u32 relay_bytes_per_second = 256 * 1024; // per introduction, each way; more is dropped
};
```

### RendezvousStats

```cpp
struct RendezvousStats {
    usize rooms = 0, sessions = 0;
    u64 introductions = 0;
    u64 relayed_packets = 0, relayed_bytes = 0;
    u64 dropped_packets = 0; // over the relay limit, or not from either side of their introduction
};
```

### Rendezvous

```cpp
class Rendezvous : NonCopyable
```

```cpp
Rendezvous();
~Rendezvous();
```

```cpp
bool start(const RendezvousSettings& settings = {});
void stop();
bool running() const;
u16 port() const;
```

Receives, introduces and relays. Call it often (a loop with a short sleep is fine). `now` is in seconds; leave it
out to use the clock.

```cpp
void update(f64 now = -1);
RendezvousStats stats() const;
```

## bowl/net/replication.hpp

`#include "bowl/net/replication.hpp"` · namespace `bowl::net`

### Messages

Plain structs as messages. Each type is reflected (BOWL_REFLECT) and registered on both ends with on&lt;T>(); it goes as
its fields (serialization, as in saves: strings, vectors, nested structs and so on all work), named by its reflected
name, so a Windows host and a Linux player agree. Messages starts every message it sends with one byte (`tag`), so
a game can send its own bytes alongside as long as they don't start with it.

```cpp
class Messages
```

```cpp
static constexpr u8 tag = 0xB1;
```

```cpp
template <reflect::Reflected T> void on(std::function<void(PeerId, const T&)> handler);
template <reflect::Reflected T> bool send(Host& host, PeerId peer, const T& message, Delivery delivery = Delivery::Reliable);
template <reflect::Reflected T> void broadcast(Host& host, const T& message, Delivery delivery = Delivery::Reliable);
```

Every Message event goes through here: true if it was one of these (its handler, if any, has run).

```cpp
bool handle(const Event& event);
```

### Replicated

Marks an entity the host shares with players. The Replicator gives it its network id; on players, the Replica's
entities have it too, with the same id.

```cpp
struct Replicated {
    u32 id = 0;
};
```

```cpp
BOWL_COMPONENT(Replicated, id)
```

How a replicated component travels. OnChange: reliably, when its value changes (health, a bag, a name: anything
that must arrive). Continuous: unreliably and often, newest wins (positions and anything else that moves).

```cpp
enum class Sync : u8 { OnChange, Continuous };
```

### ReplicationSettings

```cpp
struct ReplicationSettings {
    u32 continuous_messages = 12; // unreliable messages (up to 1 KB each) per player per update; the rest wait a turn
    u32 resend_every = 10;        // updates after which an unchanged Continuous value goes again (it may have been lost)
};
```

### Replicator

The host's side: shares every entity with a Replicated component, and the components registered with replicate(),
with each connected peer. Entity fields inside components travel too, remapped on the other end, when the entity
they point at is replicated (others arrive as nothing). Call update() once a tick, after the simulation.

```cpp
class Replicator : NonCopyable
```

```cpp
explicit Replicator(ecs::World& world, ReplicationSettings settings = {});
~Replicator();
```

```cpp
template <reflect::Reflected C> void replicate(Sync sync = Sync::OnChange);
```

Which entities a peer is shown (all, without one): for example, the ones on the map that player is on. An entity
that stops being relevant is taken away from that player, and sent again in full if it becomes relevant again.

```cpp
void relevant(std::function<bool(PeerId, ecs::Entity)> filter);
void update(Host& host);
```

```cpp
u32 net_id(ecs::Entity e) const;          // 0 if it isn't replicated (yet)
ecs::Entity entity(u32 net_id) const;     // the null entity if none
```

### Replica

A player's side: builds, updates and removes the host's replicated entities in this world, from the Replicator's
messages, with the same component registrations as the host. Entities it makes have Replicated with the host's id.

```cpp
class Replica : NonCopyable
```

```cpp
static constexpr u8 reliable_tag = 0xB2, continuous_tag = 0xB3;
```

```cpp
explicit Replica(ecs::World& world);
~Replica();
```

```cpp
template <reflect::Reflected C> void replicate(Sync sync = Sync::OnChange);
```

Every Message event goes through here: true if it was replication (and has been applied).

```cpp
bool handle(const Event& event);
```

Removes every entity it made (after leaving a game).

```cpp
void clear();
```

```cpp
ecs::Entity entity(u32 net_id) const; // the null entity if none
usize size() const;
```

## bowl/net/rollback.hpp

`#include "bowl/net/rollback.hpp"` · namespace `bowl::net`

### Rollback

Rollback: nobody waits. Each machine runs ahead with the inputs it has, guessing the missing ones (a player keeps
doing what they did last); when a real input turns out different from the guess, it goes back to the saved state of
that tick and runs forward again. A machine stops (stalled()) only when it's more than `max_rollback` ticks past
the last tick it knows every input for.

The simulation must be deterministic, and save/load must cover everything it touches. Messages carry the last
several inputs, so they can go unreliably (Delivery::Unreliable): a lost one is covered by the next.

```cpp
net::Rollback<Input, State> rollback({.players = 2, .local = me}, {
    .step = [&](u32 tick, std::span<const Input> inputs) { simulate(inputs); },
    .save = [&] { return world_state(); },
    .load = [&](const State& s) { restore(s); },
});
// every frame
while (auto e = host.receive()) if (e->type == net::Event::Type::Message) rollback.receive(e->data);
host.broadcast(rollback.advance(read_input()), net::Delivery::Unreliable);
```

```cpp
template <class Input, class State> class Rollback
```

#### Settings

```cpp
struct Settings {
    u32 players = 2;
    u32 local = 0;        // this machine's player
    u32 delay = 1;        // ticks between taking an input and using it: fewer rollbacks, a little more lag
    u32 max_rollback = 8; // how far ahead of the known inputs a machine may run
};
```

#### Game

```cpp
struct Game
```

```cpp
std::function<void(u32 tick, std::span<const Input> inputs)> step; // one tick, inputs in player order
std::function<State()> save;
std::function<void(const State&)> load;
```

```cpp
Rollback(const Settings& settings, Game game);
```

```cpp
u32 tick() const;
```

Ticks below this ran with every real input: they're final.

```cpp
u32 confirmed() const;
bool stalled() const;
u32 rollbacks() const;
u32 resimulated() const; // ticks run again, in all
```

This player's input for tick() + delay, then one tick (re-running any that guessed wrong first). Returns the
message to send the others. While stalled() it runs nothing and ignores the input, but still returns the
message so lost ones get through.

```cpp
std::vector<u8> advance(const Input& input);
```

The latest local inputs, as advance() returns them.

```cpp
std::vector<u8> message() const;
```

Another player's message. False if it isn't one.

```cpp
bool receive(std::span<const u8> bytes);
```

## bowl/net/socket.hpp

`#include "bowl/net/socket.hpp"` · namespace `bowl::net`

### Address

An IPv4 or IPv6 address and port.

```cpp
class Address
```

```cpp
Address() = default;
```

Resolves a host name or an IP literal ("example.com", "192.168.1.20", "::1"). Blocks while looking it up.

```cpp
static std::optional<Address> resolve(std::string_view host, u16 port);
static Address loopback(u16 port, bool ipv6 = false);
static Address any(u16 port, bool ipv6 = false);
```

```cpp
u16 port() const;
bool ipv6() const;
bool valid() const;
std::string to_string() const; // "192.168.1.20:27015", "[::1]:27015"
bool operator==(const Address& o) const;
```

The same on every platform, for sending an address in a packet: u8 4 or 6, the 4 or 16 address bytes, u16 port.

```cpp
void write(std::vector<u8>& out) const;
static std::optional<Address> read(std::span<const u8> in, usize& at);
```

The platform's sockaddr, for the socket code.

```cpp
const void* data() const;
void* data();
u32 size() const;
void set_size(u32 size);
```

### Socket

A non-blocking UDP socket.

```cpp
class Socket : NonCopyable
```

```cpp
Socket() = default;
~Socket();
Socket(Socket&& o) noexcept;
Socket& operator=(Socket&& o) noexcept;
```

Port 0 picks a free one (port() then says which).
loopback: only this computer can send to it (dev tools), instead of every network it's on.

```cpp
bool open(u16 port = 0, bool ipv6 = false, bool loopback = false);
void close();
bool is_open() const;
u16 port() const;
```

```cpp
bool send(const Address& to, std::span<const u8> data);
```

One waiting packet into `buffer`: its size, or nothing if none is waiting.

```cpp
std::optional<usize> receive(Address& from, std::span<u8> buffer);
```

