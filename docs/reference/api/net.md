# net

Made by `ph-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target ph_api_docs`.

- [ph/net/host.hpp](#phnethosthpp)
- [ph/net/lockstep.hpp](#phnetlockstephpp)
- [ph/net/rollback.hpp](#phnetrollbackhpp)
- [ph/net/socket.hpp](#phnetsockethpp)

## ph/net/host.hpp

`#include "ph/net/host.hpp"` · namespace `ph::net`

Connections over UDP: the same Host is a server (others connect to it) or a client (it connects to one), or both.
Messages are either reliable (delivered once, in order, resent until acknowledged; any size up to 64 KB) or
unreliable (sent once, may be lost or arrive out of order; up to 1 KB). Everything happens inside update(), on the
calling thread; there are no threads of its own.

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

### HostSettings

```cpp
struct HostSettings {
    u16 port = 0;              // 0: any free port (what clients want)
    bool ipv6 = false;         // an IPv6 socket also talks to IPv4
    u32 max_peers = 32;        // how many may connect to this host
    u32 protocol = 0x424F574C; // hosts with different numbers ignore each other: put your game's version in it
    f32 timeout = 8;           // seconds without hearing from a peer before it's gone
    f32 keep_alive = 0.25f;    // seconds between packets when there's nothing to say
    // For testing on one machine: lose this share of outgoing packets, and hold them back this long (plus up to
    // `jitter`), like a real network would.
    f32 simulated_loss = 0;
    f32 simulated_latency = 0;
    f32 simulated_jitter = 0;
};
```

### Event

```cpp
struct Event {
    enum class Type : u8 { Connected, Disconnected, Message };
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

Starts connecting; the peer's Connected (or, if nobody answers in time, Disconnected) event follows.

```cpp
PeerId connect(const Address& to);
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

## ph/net/lockstep.hpp

`#include "ph/net/lockstep.hpp"` · namespace `ph::net`

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

## ph/net/rollback.hpp

`#include "ph/net/rollback.hpp"` · namespace `ph::net`

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

## ph/net/socket.hpp

`#include "ph/net/socket.hpp"` · namespace `ph::net`

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

```cpp
bool open(u16 port = 0, bool ipv6 = false);
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

