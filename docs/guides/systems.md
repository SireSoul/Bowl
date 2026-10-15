# Systems and scheduling

A system is a function the engine runs every tick or every frame. Systems live in a `Schedule`, which runs them in
the order you added them, but runs systems that don't share data at the same time, on different cores. The result is
always the same as running them one after another.

The engine has two schedules:

| | Runs | Before |
| --- | --- | --- |
| `engine.fixed_schedule()` | every fixed tick | `Game::fixed_update` |
| `engine.frame_schedule()` | every frame | `Game::update` |

Register systems in `Game::init`.

## Kinds of systems

### Per-entity systems

```cpp
auto& fixed = engine.fixed_schedule();

// One thread, every entity with Position and Velocity:
fixed.each<Position, const Velocity>("move", [](Position& p, const Velocity& v) { p.value += v.value / 60.0f; });

// Split across threads:
fixed.par_each<Position, const Velocity>("move", [](Position& p, const Velocity& v) { /* ... */ });
```

The function can take any of these forms:

```cpp
[](Position& p, const Velocity& v) { }
[](ecs::Entity e, Position& p, const Velocity& v) { }
[](ecs::SystemContext& ctx, ecs::Entity e, Position& p, const Velocity& v) { }
```

`SystemContext` has `world`, `commands` (a command buffer for structural changes), `jobs` and `dt`, the tick or frame
length.

Both take an optional filter, and `par_each` an optional minimum chunk size (512 entities by default):

```cpp
fixed.par_each<Position>("drift", drift, ecs::without<Anchored>(), 1024);
```

### Free-form systems

```cpp
fixed.add("spawn waves", [](ecs::SystemContext& ctx) {
    // anything: queries, resources, creating entities
});
```

A free-form system is *exclusive* by default: it runs alone, after everything before it and before everything after
it. If it only touches some data, say so, and it can share a batch with other systems:

```cpp
fixed.add("steer", steer).reads<Position, Target>().writes<Velocity>();
fixed.add("score", score).reads_resource<Rules>().writes_resource<Score>();
fixed.add("rebuild grid", rebuild).exclusive(); // explicit
```

## How systems are batched

Two systems *conflict* if one writes something the other reads or writes: a component type or a resource. The
schedule walks the systems in order and groups consecutive non-conflicting ones into a batch; the systems in a batch
run in parallel, and batches run one after another.

```
"remember position"  writes Position
"wander"             writes Position, Firefly; reads Arena      conflicts with the first: next batch
"animate"            writes SpriteAnimator                       no conflict with "wander": same batch
```

Access is deduced for `each` and `par_each` from the component types (`const` means read-only). Filter components
(`with<T>`) count as reads. For `add` systems you declare it.

Getting a declaration wrong (writing something you declared as read) is a data race. When in doubt, leave a system
exclusive; correctness first, then profile.

The F1 overlay's **Systems** panel lists each system with its last run time and its batch, and has a checkbox to
switch any system off. `schedule.set_parallel(false)` runs everything on one thread, in order, which is the quickest
way to rule out a race.

## Structural changes

Systems can't create or destroy entities, or add or remove components, directly while queries run. They record them
in `ctx.commands`:

```cpp
fixed.par_each<const Health>("die", [](ecs::SystemContext& ctx, ecs::Entity e, const Health& h) {
    if (h.current <= 0) ctx.commands.destroy(e);
});
```

Commands are applied at the end of the schedule's run, in system order. Inside `par_each`, every chunk of entities
records into its own buffer, and the buffers are merged in chunk order, so the outcome doesn't depend on which thread
finished first.

To apply commands earlier, so later systems in the same run see the changes, add a barrier:

```cpp
fixed.par_each<const Health>("die", die);
fixed.barrier();                  // the destroyed entities are gone for the systems below
fixed.each<Position>("count", count);
```

The world's own buffer, `world.deferred()`, is applied by `world.flush()`, which the engine calls after
`fixed_update` and after `update`.

## Determinism

Given the same inputs, schedules produce the same component data every run, on any number of threads. Iteration order
is fixed, chunk boundaries depend only on the entity count, and commands merge in a fixed order. The tests check this
by running the same simulation with different thread counts.

One exception: entities *created* inside `par_each` chunks all get valid, unique ids, but which entity gets which id
depends on timing. Their components are still deterministic. When ids themselves must be reproducible (lockstep or
rollback networking, replays that store ids), create entities from `each` or exclusive systems.

## Managing schedules

```cpp
schedule.set_enabled("wander", false);          // skip a system by name
schedule.clear();                               // remove every system
const auto& systems = schedule.systems();       // name, access, last run time, enabled
const auto& batches = schedule.batches();       // indices into systems(), per batch
```

## Running a schedule yourself

A `Schedule` doesn't need the engine. Tools and tests can make their own:

```cpp
ecs::World world;
ecs::Schedule schedule;
JobSystem jobs;
schedule.par_each<Position, const Velocity>("move", move);
schedule.run(world, jobs, 1.0f / 60.0f);
```
