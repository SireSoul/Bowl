# Conventions

The rules the engine follows everywhere, so you can rely on them.

## Coordinates and units

- **2D, y down.** x grows to the right, y grows downward, as on screen. "Up" is negative y, so a direction of
  `-pi / 2` points up.
- **Angles are radians**, measured from +x toward +y (clockwise on screen). `radians(degrees)` and `degrees(radians)`
  convert. Tiled object rotations, which Tiled stores in degrees, are kept as degrees in `MapObject::rotation`.
- **Logical points.** Window sizes, mouse positions and screen cameras use logical points; on high-DPI displays a
  point covers several physical pixels (`window().pixel_density()`). Rendering itself happens at full resolution.
- **World units are yours.** Cameras map world units to points (`zoom` 1 means one unit per point). Most games use
  pixels of their art as world units.
- **Rectangles** are `{x, y, w, h}` with `(x, y)` the top-left corner.
- **Pivots and origins** are fractions of a size: `{0.5, 0.5}` is the center, `{0.5, 1}` the bottom center.
- **Time** is in seconds, as `f32` for deltas and `f64` for totals (`engine.time()`).

## Color

- `Color` is four floats, normally 0..1. Values above 1 are allowed and mean brighter than white (they bloom).
- You write straight (non-premultiplied) colors; the renderer premultiplies. Textures are premultiplied when loaded.
- Shaders receive and output premultiplied color.

## Names

- Namespace `bowl`, with sub-namespaces for modules whose names would be too generic otherwise: `bowl::ecs`, `bowl::gfx`,
  `bowl::audio`, `bowl::physics`, `bowl::ui`, `bowl::json`, `bowl::serial`, `bowl::reflect`.
- `snake_case` for functions, variables and files; `PascalCase` for types; `BOWL_UPPER_CASE` for macros. Member
  variables end in `_`.
- Short numeric aliases: `u8`..`u64`, `i8`..`i64`, `f32`, `f64`, `usize`.
- Getters are named after the thing (`size()`, not `get_size()`); setters are `set_...`.

## Ownership and lifetime

- The `Engine` owns every service; you get references, never ownership.
- `Asset<T>` handles share ownership of an asset. The asset stays loaded while any handle exists.
- Objects with identity (the world, the GPU device, the window) are `NonCopyable`.
- `SpriteBatch` and `Material` must stay alive until the frame they were submitted in ends; keep them as members.
- `FunctionRef` is a non-owning reference to a callable, used for callbacks that are only called during the call
  (collision queries, `parallel_for`). Don't store one.

## Threads

- The main thread owns the engine services, the asset manager, the renderer and structural changes to the world.
- Systems may run on worker threads. In `par_each`, touch only the current entity's components and data nobody writes.
- `AudioEngine`, logging and the profiler can be used from any thread.

See [architecture.md](../architecture.md#threading-rules).

## Errors

- **Programming errors** (a dead entity passed to `get`, a structural change during iteration) trip an assertion in
  dev and debug builds.
- **Runtime problems** (a missing file, a shader that won't compile, a save from a newer version) never crash. They
  log an error or add a warning (`serial::Context::warnings`), and the engine carries on with what it has: an empty
  asset handle, the previous shader, the fields it understood.
- Functions that can fail return `bool`, a pointer that may be null, or `std::optional`. The engine doesn't throw.

## Files

- Text files are UTF-8. JSON files may contain `//` and `/* */` comments and trailing commas.
- Virtual asset paths use forward slashes and no leading slash: `"maps/town.tmj"`.
- Paths in Tiled and Aseprite files are relative to the file that mentions them.
- Files the engine writes (settings, world saves) are pretty-printed JSON with keys in a stable order, so they diff
  cleanly in version control.
