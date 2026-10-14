# Architecture

```
engine/
  core/      no SDL. types, math, log, assert, jobs, profiler, file watcher, assets, JSON, UTF-8, shader bundles
  ecs/       World, Archetype, Query, CommandBuffer, Schedule              (core)
  platform/  Window, Input                                                  (core, SDL3)
  gfx/       GpuDevice, Texture, Shader, SpriteBatch, Camera2D, Renderer,   (core, SDL3, stb)
             Font + draw_text, SpriteSheet + SpriteAnimator, Tilemap
  audio/     AudioEngine, Sound, Music (streamed OGG)                       (core, SDL3, stb)
  physics/   collision shapes, sweeps, raycasts, SpatialHash, move_and_slide (core)
  app/       Engine, Game, main loop, DevTools                              (all)
  assets/    engine assets (shaders, the Roboto font), mounted as "engine/"
tools/shaderc  GLSL -> .phsh compiler (library + CLI)
samples/       sprites (ECS stress test, HUD, animation), isometric (y-sorted map, collision)
tests/         doctest unit tests (+ data/ fixtures) and an ECS benchmark
```

Each module is a static library with a public `include/ph/<module>/` folder. `ph::engine` links everything.

## The frame

SDL3 drives the loop through its callback entry points (`SDL_AppInit/Iterate/Event/Quit`). This is required on the
web, iOS and Android, so games never own `main()`.

```
events  -> Input (and the dev overlay)
iterate:
  assets.poll()                        hot reload, 4x per second (dev)
  repeat while time is owed:           fixed timestep (default 60 Hz, max 8 steps per frame)
      fixed_schedule.run -> Game::fixed_update -> world.flush
  frame_schedule.run -> Game::update -> world.flush
  renderer.begin_frame -> Game::render -> dev overlay -> renderer.end_frame
```

`Engine::alpha()` is how far between fixed ticks the frame falls. Render fixed-rate motion with
`lerp(previous, current, alpha)` and it stays smooth at any refresh rate.

## ECS

- **Components** are plain structs: no base class or code generation. Empty structs are tags and use no memory.
  Adding `PH_REFLECT(Type, field, ...)` after a struct makes it savable (JSON) and editable in the F1 inspector.
- **Storage** is by archetype: entities with the same component set share contiguous per-component arrays. Adding or
  removing a component moves the entity to another archetype. The transitions are cached as graph edges.
- **Entities** are `{index, generation}`, so a stale handle never aliases a newer entity.
- **Queries** (`world.query<A, const B>(without<C>())`) cache the archetypes they match and update incrementally.
  `const` means read-only access, which the scheduler uses.
- **Structural changes during iteration** are rejected by an assert. Record them in a `CommandBuffer` instead. Changing
  component *values*, including other entities' through `get`, is always allowed.
- **Hooks** `on_add<T>` and `on_remove<T>` handle resource cleanup. **Resources** are typed singletons on the world.

### Scheduler

`Schedule::par_each<Ts...>` and `each<Ts...>` register systems whose access comes from their signature.
`add(name, fn)` systems are exclusive unless they declare `.reads<>()`, `.writes<>()` or the `_resource` variants.
Consecutive systems that don't conflict run in the same parallel batch. Systems that conflict keep their insertion order.
The result is always identical to running them one by one.

Commands recorded by systems are applied in system order at `barrier()` points and at the end of `run()`. Inside
`par_each`, each chunk of entities records into its own buffer. Chunk boundaries depend only on entity count, and the
buffers merge in chunk order.

**Determinism:** component data and iteration order don't depend on thread count. The test suite checks this. One
exception: entity ids created inside `par_each` chunks are unique, but which id each new entity gets depends on thread
timing. When ids must be reproducible (lockstep or rollback netcode), spawn from `each()` or exclusive systems.
Removing this limitation is on the roadmap.

### Saving, loading, prefabs

```cpp
struct Health { i32 current = 100; i32 max = 100; ecs::Entity attacker; };
PH_REFLECT(Health, current, max, attacker)

serial::Context ctx{&engine.assets()};
json::Value save = ecs::save_world(world, ctx);              // every entity with savable components
auto spawned = ecs::load_entities(other_world, save, ctx);   // adds to a world; returns the new entities
ecs::Entity guard = ecs::spawn_prefab(world, prefab_json, ctx);
```

- Savable components: reflected ones, ones with `ph_to_json`/`ph_from_json` hooks, and tags. Others are skipped, with a
  warning in `ctx.warnings`.
- Fields can be numbers, bools, enums, strings, math types, `Color`, vectors, arrays, optionals, nested reflected
  structs, `Asset<T>` (saved as the path, reloaded on load), and `Entity`. Entity references are remapped, so they
  survive a load into a world that already has entities.
- Loading is tolerant: missing fields keep their defaults, and unknown components are skipped with a warning. Old save
  files keep loading as components gain fields.
- Components are saved by type name. A type has to be known before loading (`ecs::register_components<A, B>()`, or by
  having been used).

## Rendering

The renderer is SDL_GPU: Metal on macOS and iOS, Vulkan on Linux, Android and Windows, and D3D12 on Windows (and
Xbox). Metal and D3D12 have both rendered the samples (D3D12 on CI, via WARP); `--driver vulkan|metal|direct3d12`
picks one explicitly.

- **Sprites** use vertex pulling. Each sprite is a 64-byte record in a storage buffer, and the vertex shader builds the
  quad from `gl_VertexIndex`, so there are no vertex buffers. All sprites of a frame go up in one upload, with one
  draw call per texture run. Batches sort by depth (isometric: use the feet's y), then by texture.
- **Blending** uses premultiplied alpha everywhere. Textures are premultiplied on load. Each batch picks a `BlendMode`
  (alpha, additive, multiply, opaque).
- **Materials:** a batch can use its own fragment shader (`gfx::Material`), with up to three extra textures and a
  256-byte uniform block. Pipelines are created on first use and cached per (shader, target format, blend). A
  hot-reloaded shader simply gets a new pipeline.

A frame flows through these steps:

```
submit(batch)        -> scene (RGBA16F, HDR: sprite colors above 1 are allowed and glow with bloom)
submit_light(batch)  -> light map (half resolution, additive)           [post().lighting]
                        bloom: bright-pass, 6-level downsample + tent upsample chain  [post().bloom]
final pass           -> scene * (ambient + light) + bloom, exposure, tonemap, saturation, contrast, tint, vignette
submit_ui(batch)     -> drawn on the final image, untouched by lighting and post
dev overlay          -> on top; --screenshot reads back this image; then it's blitted to the window
```

With default `PostSettings` the final pass is an exact copy, so games pay for effects only when they turn them on.
Everything is tunable live in the overlay's "Post & lighting" panel.

## Shaders

Shaders are written once in Vulkan GLSL as `name.vert.glsl` / `name.frag.glsl`, and `#include` is supported.
`ph-shaderc` compiles them to `.phsh` bundles containing SPIR-V, MSL and HLSL (Shader Model 5.1) plus the resource
counts SDL_GPU needs. It remaps Metal slots and HLSL registers to SDL's layout, and if a resource is in the wrong
descriptor set it reports which set to use. On Windows the D3D12 backend compiles the HLSL to DXBC at load time with
the system's `d3dcompiler_47.dll`.

Required descriptor sets (SDL_GPU's model):

| stage | textures, storage textures, storage buffers | uniform buffers |
| --- | --- | --- |
| vertex | set 0 | set 1 |
| fragment | set 2 | set 3 |

The build precompiles every shader. Dev builds also link the compiler, so the asset system compiles `.glsl` at load
and on every save.

## Assets

`AssetManager` maps virtual paths to mounted folders, checking earlier mounts first. Dev builds mount the source
folders before the build output, so edits hot-reload. `Asset<T>` handles are shared: a reload swaps the value in place
and bumps `version()`. A failed reload keeps the old value. Loaders read through `AssetLoadContext`, which records every
file touched (including shader `#include`s) as a reload trigger. Assets made in code go through `assets.add(path, ptr)`.

Built-in asset types:

| Type | Files | Notes |
| --- | --- | --- |
| `gfx::Texture` | .png .jpg .tga .bmp | premultiplied on load; mipmapped unless the game is pixel art |
| `gfx::Shader` | .vert/.frag (.glsl or .phsh) | load by path without extension |
| `gfx::Font` | .ttf .otf | glyphs rasterized on demand |
| `gfx::SpriteSheet` | Aseprite .json | loads its PNG too |
| `gfx::Tilemap` | Tiled .tmj (+ .tsj) | loads tileset and image-layer PNGs too; edits to the map, its tilesets or images hot-reload |
| `audio::Sound` | .wav .ogg | fully decoded, for effects |
| `audio::Music` | .ogg | streamed |

At runtime, `ph_add_game(target ASSETS dir)` lays out next to the executable:

```
<exe dir>/engine/   engine assets + compiled shaders
<exe dir>/assets/   game assets + compiled shaders
```

## Threading rules

- Main thread: Engine services, AssetManager, renderer, world structure.
- Systems run on worker threads. Inside `par_each`, touch only the current entity's components plus read-only data.
- `AudioEngine` is thread-safe. Mixing happens on SDL's audio thread.
- Logging and the profiler are thread-safe.
