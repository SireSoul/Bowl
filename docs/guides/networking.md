# Networking

`ph_net` connects games over UDP. `net::Host` makes connections and carries messages; `net::Lockstep` and
`net::Rollback` keep a simulation in step across machines. Everything runs on your thread, inside calls you make:
there are no network threads and no callbacks.

## Connections

The same `Host` is a server (it takes connections), a client (it makes one), or both:

```cpp
#include "ph/net/host.hpp"

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
#include "ph/net/lockstep.hpp"

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
#include "ph/net/rollback.hpp"

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

`ph_net` doesn't do encryption or authentication (the connection key only stops strangers from mixing in packets),
NAT punch-through, relays, lobbies or matchmaking. Over the internet, players need a reachable host (an open port, or
a server with a public address), or a service such as Steam networking.

Multiplayer that sends state instead of inputs (a server runs the game and sends what changed) is built from `Host`
messages and the game's own serialization. There's no replication system for it yet.
