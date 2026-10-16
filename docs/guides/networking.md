# Networking

`bowl_net` connects games over UDP. `net::Host` makes connections and carries messages; `net::Lockstep` and
`net::Rollback` keep a simulation in step across machines. Everything runs on your thread, inside calls you make:
there are no network threads and no callbacks.

## Connections

The same `Host` is a server (it takes connections), a client (it makes one), or both:

```cpp
#include "bowl/net/host.hpp"

net::Host server;
server.start({.port = 27015});

net::Host client;
client.start(); // any free port
net::PeerId to_server = client.connect(*net::Address::resolve("192.168.1.20", 27015));
```

Call `update()` every frame, then read what happened:

```cpp
host.update();
while (auto e = host.receive()) {
    switch (e->type) {
    case net::Event::Type::Connected: /* e->peer is new */ break;
    case net::Event::Type::Disconnected: /* e->peer left, timed out, or (as a client) was turned away */ break;
    case net::Event::Type::Message: /* e->data from e->peer */ break;
    }
}
```

`connect` returns the peer's id right away. Its `Connected` event follows when the other side answers; if nobody
answers within the timeout, a `Disconnected` event follows instead. A host that already has `max_peers` connections
turns new ones away. `disconnect(peer)` says goodbye, and `stop()` says it to everyone.

`HostSettings::protocol` is a number both sides must share: hosts with different numbers ignore each other. Put your
game's version in it so old and new builds don't talk.

### Encryption

Every connection is encrypted; there's nothing to turn on. When two hosts connect they agree on fresh keys (X25519,
from [Monocypher](https://monocypher.org)), one per direction, and every packet after that is encrypted and
authenticated (XChaCha20-Poly1305). Nobody on the network in between can read a message, change one, send an old
packet again, or forge a goodbye that drops a player.

What a key exchange alone can't stop is someone who intercepts the handshake itself and talks to each side in the
other's place. `HostSettings::secret` closes that: the keys depend on it, so hosts with different secrets can't
connect, and a host lets in only peers that know its secret.

```cpp
server.start({.port = 27015, .secret = "correct horse battery staple"}); // a private game's password
client.start({.secret = "correct horse battery staple"});
```

A secret built into the game keeps out other programs and changed builds that don't have it, though anyone who digs
it out of the game can use it. For a private match, let players type a password and use that.

A server reports a client as `Connected` only after a packet that shows the client has the keys, so peers with the
wrong secret never appear. It counts toward `max_peers` until it times out.

## Rooms: playing over the internet

Most players are behind home routers that don't let connections in, so one player hosting with `connect()` only
works if they open a port. Rooms get around that with a rendezvous server: a small program, `bowl-rendezvous`, on a
machine everyone can reach (any cheap server with a public address and its UDP port open; it uses little power).

```bash
bowl-rendezvous --port 27016
```

The host names a room and the player joins it by name:

```cpp
auto server = *net::Address::resolve("rendezvous.example.com", 27016);

host.host_room(server, room_code);           // keeps telling the server the room is there, from update()
net::PeerId peer = player.join_room(server, room_code);
// then the same Connected event as connect(), or Disconnected if nobody hosts the room
```

The server introduces the two, and they connect straight to each other: both send first, which opens both routers
(NAT punch-through). Some routers won't allow that; then, after two seconds, the player goes through the server's relay
instead, and `stats(peer).relayed` says so. `net::Route::Relay` always takes the relay, so the players never learn each
other's addresses.

Either way the connection is the same encrypted one, so the server only passes on packets it can't read. It forgets a
room a few seconds after its host goes quiet (`stop_hosting_room()` or `stop()`), and lets each relayed connection pass
256 KB a second each way (`--relay-kbps` changes that). Room names are up to 64 bytes. Anyone who knows a room's name
can join it, so make codes hard to guess (random letters) unless the room is meant to be public, and add a `secret` for
a private game. Your game can also run the server itself: `net::Rendezvous` is the same thing as a class.

To test the relay on one machine, `HostSettings::simulated_unreachable` makes a host act as if no one can reach it
directly, so joining it has to take the relay.

## Messages

```cpp
host.send(peer, bytes);                                // reliable
host.send(peer, bytes, net::Delivery::Unreliable);
host.broadcast(bytes);                                 // every connected peer
```

| | Reliable | Unreliable |
| --- | --- | --- |
| Arrives | once, in the order sent | at most once, maybe out of order |
| Lost packets | sent again until acknowledged | gone |
| Size | up to 64 KB (cut into pieces and put back together) | up to 1 KB |
| For | chat, joining, game events, lockstep inputs | positions, rollback inputs: anything the next message replaces |

Messages are bytes. For plain structs, `net::to_bytes(value)` and `net::from_bytes<T>(data)` do the copying; for
anything else, write it with the serialization in [Saving and loading](saving.md). The bytes go as they are, so plain
structs must have the same layout on every machine (fixed-size types like `i32` and `f32`, no pointers).

`host.stats(peer)` has the round-trip time, recent packet loss, and packet counts.

### Testing on one machine

Two copies of a game on one machine see a perfect network. `HostSettings` can make it worse:

```cpp
server.start({.port = 27015, .simulated_loss = 0.1f, .simulated_latency = 0.05f, .simulated_jitter = 0.02f});
```

This loses 10% of the packets the host sends, and holds the rest back 50–70 ms. Turn it on on both sides.

## Lockstep

In lockstep, every machine runs the same simulation from the same inputs. A tick runs only once every player's input
for it has arrived, so the machines never disagree, and the only thing sent is input. Inputs are taken a few ticks
ahead (`delay`) so they have time to arrive. If one player's input is late, everyone waits.

```cpp
#include "bowl/net/lockstep.hpp"

struct Input { // a plain struct with an ==
    i8 move_x = 0, move_y = 0;
    u8 buttons = 0;
    bool operator==(const Input&) const = default;
};

net::Lockstep<Input> lockstep(player_count, my_player, /*delay=*/2);

// in fixed_update
if (auto m = lockstep.local(read_input())) host.broadcast(*m); // reliable
while (auto e = host.receive())
    if (e->type == net::Event::Type::Message) lockstep.receive(e->data);
if (lockstep.ready()) {
    simulate(lockstep.inputs()); // one Input per player, in player order
    lockstep.advance();
}
```

`waiting_for()` lists the players holding things up, for a "waiting for…" message.

Lockstep suits games with many units and little input, like strategy games, where sending state would cost too much.
The delay is felt on every input, so it suits slower games better than fast ones.

## Rollback

With rollback, nobody waits. Each machine runs ahead with the inputs it has, guessing the missing ones: a player is
assumed to keep doing what they did last. When a real input arrives and differs from the guess, the machine loads
its saved state from that tick and runs forward again with the right input, all within one frame. The player sees
their own input with no delay, at the cost of the occasional correction.

```cpp
#include "bowl/net/rollback.hpp"

net::Rollback<Input, GameState> rollback({.players = 2, .local = my_player, .delay = 1, .max_rollback = 8}, {
    .step = [&](u32 tick, std::span<const Input> inputs) { simulate(inputs); },
    .save = [&] { return state; },          // everything the simulation touches
    .load = [&](const GameState& s) { state = s; },
});

// in fixed_update
while (auto e = host.receive())
    if (e->type == net::Event::Type::Message) rollback.receive(e->data);
host.broadcast(rollback.advance(read_input()), net::Delivery::Unreliable);
```

`advance` takes this tick's input, fixes any wrong guesses, runs one tick, and returns the message to send. Each
message repeats the last several inputs, so unreliable delivery is fine: a lost message is covered by the next one.

A machine never runs more than `max_rollback` ticks past the last tick it has every input for. Past that, it's
`stalled()`: `advance` runs nothing until inputs arrive, but still returns a message so the others hear from it.
Ticks below `confirmed()` ran with real inputs only and won't change, so use them for things that must not be undone,
like playing a sound once or saving a replay.

`save` and `load` run every tick, so keep the state small, or save only what the simulation changes. A rollback runs
up to `max_rollback` ticks again in one frame, so a tick must be cheap enough to run that many times.

Rollback suits fighting games and other fast games with few players and small state.

## Determinism

Both lockstep and rollback need the same inputs to give the same result on every machine:

- Run the simulation in `fixed_update` (see [The game loop](game-loop.md)), never with the frame's `dt`.
- Use one seeded random generator in the game state, never `rand()` or the clock.
- Don't let iteration order depend on pointers or hash maps. ECS queries and the schedule are deterministic, including
  parallel systems and the ids of entities they spawn (see [Systems and scheduling](systems.md#determinism)).
- Floating point gives the same results for the same build on the same kind of CPU. Across compilers, operating
  systems or CPU architectures, it may not. If players on different platforms play together, use fixed-point math
  (integers) in the simulation.

To catch a desync, hash the state of a confirmed tick now and then, and compare the hashes across machines.

## What it doesn't do

`bowl_net` doesn't do lobbies or matchmaking: listing rooms, or putting strangers together, is the game's (or a
service's) job; rooms join players who share a name. Over the internet, players need a reachable host (an open port, or
a server with a public address), or a service such as Steam networking.

Multiplayer that sends state instead of inputs (a server runs the game and sends what changed) is built from `Host`
messages and the game's own serialization. There's no replication system for it yet.
