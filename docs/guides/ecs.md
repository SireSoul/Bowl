# ECS

Game objects are *entities*: ids with *components* attached. Components are plain structs holding data; *systems*
are functions that run over every entity with a given set of components. This page covers the world, entities,
components, queries, command buffers, hooks, resources and the transform hierarchy. Scheduling systems has its own
page: [systems.md](systems.md).

```cpp
#include "ph/ecs/ecs.hpp" // included by ph/app/main.hpp already
```

- [Entities](#entities)
- [Components](#components)
- [Reading and writing components](#reading-and-writing-components)
- [Queries](#queries)
- [Changing structure while iterating](#changing-structure-while-iterating)
- [Hooks](#hooks)
- [Resources](#resources)
- [Parent and child transforms](#parent-and-child-transforms)
- [How it's stored](#how-its-stored)
- [Limits](#limits)

## Entities

```cpp
ecs::World& world = engine.world();

ecs::Entity tree = world.create();                                 // no components yet
ecs::Entity rock = world.create(Position{{10, 20}}, Solid{});       // with components
world.destroy(rock);
bool still_there = world.alive(rock);                               // false
```

An `ecs::Entity` is a small value (index and generation, 8 bytes). Copy it and store it freely. When an entity is
destroyed, its slot's generation changes, so old handles to it are recognized as dead instead of silently pointing at
whatever reuses the slot. `ecs::Entity{}` (or `ecs::null_entity`) is the null entity and converts to `false`.

`world.entity_count()` is the number of live entities.

## Components

Any struct that can be moved and destroyed is a component. No base class, no registration step:

```cpp
struct Velocity {
    Vec2 value;
};
struct Frozen {}; // an empty struct is a tag: it takes no memory per entity
```

Adding `PH_COMPONENT` after a component lists its fields:

```cpp
struct Health {
    i32 current = 100;
    i32 max = 100;
};
PH_COMPONENT(Health, current, max)
```

That's optional, and buys three things:

- the component can be [saved and loaded](saving.md) with the world, and in prefabs;
- the F1 inspector shows its fields and lets you edit them while the game runs;
- it survives [game-code hot reload](hot-reload.md#game-code).

`PH_COMPONENT` goes at namespace scope, in the same namespace as the struct, and takes up to 32 fields. For structs
that are only *fields* of components (not components themselves), use `PH_REFLECT` with the same syntax.

## Reading and writing components

```cpp
world.add<Velocity>(e, Vec2{30, 0});            // add, or replace if it's already there; returns a reference
world.insert(e, Health{50, 100}, Frozen{});     // add or replace several at once (one move in memory)
world.remove<Frozen>(e);

bool frozen = world.has<Frozen>(e);
Health* h = world.try_get<Health>(e);           // nullptr if missing or the entity is dead
Health& sure = world.get<Health>(e);            // asserts if missing
```

Prefer `insert` over several `add` calls when giving an entity several components at once: each `add` moves the
entity to a new archetype (see [How it's stored](#how-its-stored)), `insert` moves it once.

## Queries

`world.each` runs a function over every entity that has all the listed components:

```cpp
world.each<Position, const Velocity>([](Position& p, const Velocity& v) { p.value += v.value * dt; });

// Take the entity as the first parameter when you need it:
world.each<const Health>([&](ecs::Entity e, const Health& h) {
    if (h.current <= 0) world.deferred().destroy(e);
});
```

Mark components you only read as `const`. It documents intent, and the scheduler uses it to decide which systems can
run at the same time.

### Filters

Require or exclude components you don't need to read:

```cpp
auto q = world.query<Position>(ecs::with<Player>());              // entities with Position and Player
auto r = world.query<Position>(ecs::without<Frozen>());           // with Position, without Frozen
auto s = world.query<Position>(ecs::QueryFilter{}.require<Enemy>().exclude<Dead>());
```

### Keeping a query

`world.each` builds a query each call. A `Query` object caches the matching archetypes and picks up new ones as they
appear, so keep it when you run the same query every frame (systems do this for you):

```cpp
ecs::Query<Position, const Velocity> movers = world.query<Position, const Velocity>();

movers.each([](Position& p, const Velocity& v) { /* ... */ });
usize n = movers.count();
ecs::Entity any = movers.first();    // null if there are none
```

### In parallel

```cpp
movers.par_each(engine.jobs(), [](Position& p, const Velocity& v) { p.value += v.value * dt; });
```

`par_each` splits the entities into chunks and runs them on the job system's workers. The function may only touch
the entity it's given (plus data nobody writes). Inside a schedule, use `Schedule::par_each` instead, which also
gives each chunk its own command buffer: see [systems.md](systems.md).

For hand-tuned loops, `each_archetype` hands you the raw component arrays of each archetype:

```cpp
movers.each_archetype([](std::span<const ecs::Entity> entities, Position* p, const Velocity* v) {
    for (usize i = 0; i < entities.size(); ++i) p[i].value += v[i].value * dt;
});
```

## Changing structure while iterating

Creating or destroying entities, and adding or removing components, are *structural changes*: they can move entities
between archetypes. Doing that in the middle of iterating would skip or repeat entities, so the world asserts if you
try.

Record structural changes in a `CommandBuffer` instead; they're applied later, in the order you recorded them:

```cpp
world.each<const Health>([&](ecs::Entity e, const Health& h) {
    if (h.current <= 0) {
        world.deferred().destroy(e);
        world.deferred().create(Position{/* ... */}, Corpse{});
    }
});
```

`world.deferred()` is applied by `world.flush()`, which the engine calls after the fixed tick and after the frame
update. Systems get their own buffer as `ctx.commands` (see [systems.md](systems.md)).

What you *can* do while iterating: read and change component values of any entity, including others through
`get`/`try_get`.

A command buffer can also:

```cpp
ecs::Entity e = cmd.create();                 // the id is valid right away; the entity exists once applied
cmd.add<Velocity>(e, Vec2{1, 0});
cmd.insert(e, Health{}, Enemy{});
cmd.remove<Frozen>(other);
cmd.run([](ecs::World& w) { /* any deferred work, in order with the rest */ });
```

Commands aimed at an entity that died before the buffer was applied are skipped.

## Hooks

Hooks run when a component arrives on or leaves an entity:

```cpp
world.on_add<Collider>([](ecs::World& w, ecs::Entity e) { /* register with a physics broadphase */ });
world.on_remove<Collider>([](ecs::World& w, ecs::Entity e) {
    const Collider& c = w.get<Collider>(e); // still readable here
    /* unregister */
});
```

`on_remove` also runs when the entity is destroyed. Hooks run with the world locked, so structural changes inside a
hook go through `w.deferred()`.

## Resources

A resource is a value stored once per world, found by its type. Use them for shared state such as the map, the
score, a random generator or a spatial hash.

```cpp
struct Score { int points = 0; };

world.set_resource<Score>();                    // construct (arguments are forwarded)
world.resource<Score>().points += 10;           // asserts if missing
if (Score* s = world.try_resource<Score>()) {}  // nullptr if missing
world.remove_resource<Score>();
```

The engine stores a pointer to itself as a resource, so systems can reach engine services:
`ctx.world.resource<Engine*>()`.

## Parent and child transforms

`ph/ecs/hierarchy.hpp` adds a scene-graph style hierarchy on top of the ECS:

```cpp
#include "ph/ecs/hierarchy.hpp"

ecs::Entity ship = world.create(ecs::Transform2D{{100, 100}});
ecs::Entity turret = world.create(ecs::Transform2D{{0, -12}});
ecs::set_parent(world, turret, ship);              // the turret now moves and rotates with the ship

ecs::add_transform_systems(engine.frame_schedule()); // once, in init
```

- `Transform2D` is position, rotation and scale relative to the parent (or the world, without one).
- `GlobalTransform2D` is the result in world space, written by the transform system. Read it when drawing;
  `global.apply(local_point)` transforms a point.
- `set_parent(world, child, parent)` reparents, refusing (returning `false`) if it would create a cycle. Pass a null
  parent to detach.
- `destroy_recursive(world, root)` destroys an entity and everything under it.
- `Parent` and `Children` components record the links. All of these are reflected, so hierarchies save and load.

From inside a system, reparent through a command: `ctx.commands.run([=](ecs::World& w) { ecs::set_parent(w, c, p); })`.

## How it's stored

Entities with exactly the same set of components live together in an *archetype*: a table with one tightly packed
array per component. A query visits the archetypes that match and walks their arrays front to back, which is about
as fast as memory access gets.

The cost is on structural changes: adding or removing a component moves the entity's data to another archetype.
That's cheap (the moves between archetypes are cached), but avoid toggling components every tick on thousands of
entities; a `bool` field is often better than a tag that comes and goes.

`ph_bench_ecs` (in `tests/`) measures creation, iteration and add/remove on a million entities.

## Limits

- 256 component types by default. Define `PH_ECS_MAX_COMPONENTS` (a multiple of 64) to raise it.
- Archetypes are never freed; fine for games, which have a bounded set of component combinations.
- Entities created inside `par_each` get ids that depend on thread timing (their data doesn't); see
  [systems.md](systems.md#determinism).
