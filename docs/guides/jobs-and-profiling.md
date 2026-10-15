# Jobs, logging and profiling

## The job system

`engine.jobs()` is a pool of worker threads, one per hardware thread minus the main thread by default
(`AppConfig::worker_threads`). The ECS scheduler uses it for parallel systems; you can use it directly for anything
else that splits into independent pieces: pathfinding requests, procedural generation, decoding.

### parallel_for

```cpp
std::vector<Tile> tiles(1'000'000);
engine.jobs().parallel_for(tiles.size(), 4096, [&](usize begin, usize end) {
    for (usize i = begin; i < end; ++i) tiles[i] = generate(i);
});
// every chunk has finished here
```

The range is split into contiguous chunks of at least `min_chunk` items (4096 here). Chunk boundaries depend only on
the count and `min_chunk`, never on the number of threads, so results collected per chunk can be merged in a
deterministic order. `JobSystem::chunk_size(count, min_chunk)` tells you the chunk size it will use.

### Tasks

```cpp
TaskGroup group;
engine.jobs().run(group, [&] { build_navmesh(level_a); });
engine.jobs().run(group, [&] { build_navmesh(level_b); });
engine.jobs().wait(group);   // the waiting thread runs queued jobs too, instead of sleeping
```

A pool with zero workers still works: everything runs on the waiting thread. That's how single-core targets and some
tests run.

### Rules

- Jobs must not touch engine services that are main-thread only: the asset manager, the renderer, structural changes
  to the world. Reading component data, math and your own data structures is fine.
- `wait` before using the results; the lambda's captures must outlive the jobs.
- Jobs are for CPU work. They're not a way to do I/O in the background while frames continue: `wait` blocks.

## Logging

```cpp
ph::info("loaded {} enemies from {}", count, path);   // std::format syntax
ph::warn("save slot {} is from a newer version", slot);
ph::error("can't open {}", file.string());
ph::debug("path cost {}", cost);
ph::trace("...");
```

Levels, from least to most severe: `Trace`, `Debug`, `Info`, `Warn`, `Error`. Lines below the current level
(`AppConfig::log_level`, `Info` by default) aren't even formatted, so leaving `debug` calls in costs almost nothing.
`logging::set_level` changes it at runtime.

Lines go to the console with the time since startup, and to the overlay's **Log** panel.
`logging::add_sink(fn, user)` sends them anywhere else (a file, a network console); sinks are called from whichever
thread logged, one at a time. Logging is thread-safe.

## Assertions

```cpp
PH_ASSERT(index < size);
PH_ASSERT(texture, "the hero texture must be loaded before spawning");
```

A failed assertion logs the condition, the message and the location, then stops the program (in the debugger, if
one is attached). Assertions are on in debug builds and in any build with dev tools, and compiled out of release
builds; define `PH_ASSERTS` as 0 or 1 to override. `PH_VERIFY(cond)` is the same, but `cond` is still evaluated when
assertions are off. `PH_UNREACHABLE()` marks code that must never run.

The engine uses assertions for programming errors, never for things that can go wrong at runtime like missing files.

## Profiling

The CPU profiler is always compiled in and cheap enough to leave on. Mark the code you want measured:

```cpp
void rebuild_navigation() {
    PH_PROFILE_SCOPE("rebuild navigation");
    // ...
}
```

Zones with the same name add up per frame, across calls and threads. The engine already measures its own work
(`fixed tick`, `frame update`, `game render`, `assets.poll`, and every system by name).

The overlay's **Profiler** panel shows the last frame's zones, sorted by time, with call counts; the graph at the top
shows recent frame times. From code: `profiler::last_frame()` returns the zones and `profiler::frame_history()` the
frame times.

For timelines (when did each thread do what), a Tracy integration is on the [roadmap](../roadmap.md).

## Timing small things

`ph/core/time.hpp` has `now_seconds()` and a `Stopwatch` for quick measurements:

```cpp
Stopwatch watch;
expensive();
info("took {:.2f} ms", watch.milliseconds());
```
