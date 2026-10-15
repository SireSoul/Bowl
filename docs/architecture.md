# Architecture

How the engine is put together, what happens in a frame, and the rules every part follows. The
[guides](README.md#guides) go deeper into each system.

- [Modules](#modules)
- [The frame](#the-frame)
- [ECS](#ecs)
- [Game-code hot reload](#game-code-hot-reload)
- [Rendering](#rendering)
- [Shaders](#shaders)
- [Assets](#assets)
- [Threading rules](#threading-rules)
- [Design principles](#design-principles)

## Modules

```
engine/
  core/      types, math, easing, log, assert, jobs, profiler, files, file watcher,     (no dependencies)
             assets, JSON, UTF-8, reflection, serialization, settings, localization
  ecs/       World, Archetype, Query, CommandBuffer, Schedule, hierarchy, world I/O    (core)
  platform/  Window, Input                                                              (core, SDL3)
  gfx/       GpuDevice, Texture, Shader, SpriteBatch, Camera2D, Renderer, Material,    (core, ecs, SDL3, stb)
             Font + draw_text, SpriteSheet + SpriteAnimator, Tilemap, particles
  audio/     AudioEngine, Sound, Music (streamed OGG)                                   (core, SDL3, stb)
  physics/   collision shapes, sweeps, raycasts, SpatialHash, move_and_slide            (core)
  ui/        immediate-mode game UI, Typewriter                                          (gfx, platform)
  app/       Engine, Game, Scene stack, main loop, DevTools, HotReloadHost              (everything)
  assets/    engine assets (shaders, the Roboto font), mounted as "engine/"
tools/shaderc  GLSL -> .phsh compiler (library + CLI)
```

Each module is a static library, `ph_<module>` (alias `ph::<module>`), with its public headers in
`engine/<module>/include/ph/<module>/`. `ph::engine` is an interface target that links all of them; games link that,
usually through `ph_add_game`.

The dependency arrows only point down the list. `core` knows nothing about SDL or the GPU, which is why most of it is
tested without a window. Within a module, headers include only what they use, and platform types (`SDL_Window`,
`SDL_Event`) are forward-declared where possible.

## The frame

SDL3 drives the loop through its callback entry points (`SDL_AppInit`, `SDL_AppIterate`, `SDL_AppEvent`,
`SDL_AppQuit`). The web, iOS and Android need those, and using them everywhere keeps the game code identical on every
platform, so games never write `main()`; `PH_MAIN(YourGame)` does it.

Startup:

```
configure()                         the game returns an AppConfig: window, fixed rate, threads, pixel art, ...
SDL, settings, jobs, window, GPU, audio
mount assets, register loaders      dev builds mount the source folders first
renderer, camera, dev tools
Game::init()                        systems, input bindings, assets, resources
saved controls applied              player rebinds override the game's defaults
Game::start()                       create the world
```

Every frame:

```
events -> Input (and the dev overlay)
iterate:
  assets.poll()                          hot reload, 4 times a second (dev builds)
  while simulation time is owed:         fixed timestep, 60 Hz by default, at most 8 ticks per frame
      fixed_schedule.run -> Game::fixed_update -> scenes.fixed_update -> world.flush
  frame_schedule.run -> Game::update -> scenes.update -> world.flush
  renderer.begin_frame -> Game::render -> scenes.render -> dev overlay -> renderer.end_frame
  scene changes requested this frame are applied
```

`Engine::alpha()` is how far the frame falls between the last two fixed ticks. Draw fixed-rate motion at
`lerp(previous, current, alpha)` and it stays smooth at any refresh rate. If a frame takes so long that more than
`max_fixed_steps` ticks are owed, the extra time is dropped instead of letting the game fall further behind.

[guides/game-loop.md](guides/game-loop.md) covers each callback.

## ECS

- **Components** are plain structs: no base class, no code generation. Empty structs are tags and use no memory.
  `PH_COMPONENT(Type, fields...)` (or `PH_REFLECT`) after a struct makes it savable as JSON and editable in the
  inspector.
- **Storage** is by archetype: entities with exactly the same set of components share one table, with one tightly
  packed array per component. Adding or removing a component moves the entity to another archetype; those moves are
  cached as edges of an archetype graph.
- **Entities** are `{index, generation}`, so a stale handle never refers to a newer entity that reused the slot.
- **Queries** (`world.query<A, const B>(ecs::without<C>())`) cache the archetypes they match and pick up new ones as
  they appear. `const` marks read-only access, which the scheduler uses.
- **Structural changes during iteration** trip an assert. Record them in a `CommandBuffer` instead. Changing component
  *values*, including other entities' through `get`, is always allowed.
- **Hooks** (`on_add<T>`, `on_remove<T>`) react to components arriving and leaving. **Resources** are typed singletons
  on the world.

### Scheduler

`Schedule::each<Ts...>` and `par_each<Ts...>` register systems whose access is deduced from their component types.
Systems added with `add(name, fn)` are exclusive unless they declare `.reads<>()`, `.writes<>()` or the `_resource`
variants. Consecutive systems that don't conflict run in the same parallel batch; systems that conflict keep their
insertion order. The result is always the same as running them one by one.

Commands recorded by systems are applied in system order at `barrier()` points and at the end of `run()`. Inside
`par_each`, each chunk of entities records into its own buffer. Chunk boundaries depend only on the entity count, and
the buffers are merged in chunk order.

**Determinism:** component data and iteration order don't depend on the number of threads, and the tests check that.
One exception: entities created inside `par_each` chunks get unique ids, but which id each one gets depends on
thread timing. When ids must be reproducible (lockstep or rollback netcode), create entities from `each()` or from
exclusive systems. Lifting this limit is on the roadmap.

### Saving, loading, prefabs

```cpp
struct Health { i32 current = 100; i32 max = 100; ecs::Entity attacker; };
PH_COMPONENT(Health, current, max, attacker)

serial::Context ctx{&engine.assets()};
json::Value save = ecs::save_world(world, ctx);              // every entity with savable components
auto spawned = ecs::load_entities(other_world, save, ctx);   // adds to a world; returns the new entities
ecs::Entity guard = ecs::spawn_prefab(world, prefab_json, ctx);
```

- Savable components are reflected ones, ones with `ph_to_json`/`ph_from_json` hooks, and tags. Others are skipped,
  with a warning in `ctx.warnings`.
- Fields can be numbers, bools, enums, strings, math types, `Color`, vectors, arrays, optionals, nested reflected
  structs, `Asset<T>` (saved as its path and loaded again) and `Entity`. Entity references are remapped, so they
  survive loading into a world that already has entities.
- Loading is tolerant: missing fields keep their defaults and unknown components are skipped with a warning, so old
  save files keep loading as components gain fields.
- Components are saved by type name. A type has to be registered before loading: `PH_COMPONENT` does that when the
  program starts; otherwise call `ecs::register_components<A, B>()`.

[guides/ecs.md](guides/ecs.md), [guides/systems.md](guides/systems.md) and [guides/saving.md](guides/saving.md) have
the full story.

## Game-code hot reload

With `ph_add_game(target ... HOT_RELOAD)` (dev builds, CMake 3.27+), the game's sources are built into `target_code`,
a loadable library. `target` becomes a host executable that contains the whole engine and exports its symbols. On
macOS and Linux the library links against nothing and finds engine functions in the host when it's loaded; on Windows
it links against the host's import library. `PH_MAIN` turns into the library's `ph_create_game` export. The host also
watches the game's source folders and rebuilds the library when a file is saved.

When the library file changes and the linker has finished writing it, the host (`ph::HotReloadHost`):

1. saves the world with `save_world`;
2. tears down everything the old code made, while that code is still loaded: scenes, systems, the world (entities,
   hooks, resources), and every asset, loader and callback registered since startup (`AssetManager::mark/rollback`);
3. marks component registrations whose code lived in the old library as dormant;
4. loads a fresh copy of the new library, whose `PH_COMPONENT` registrations refresh those entries under the same ids;
5. creates the new `Game`, runs `init()`, applies saved controls again and loads the world back.

If the new library won't load, the old registrations are restored and the old code comes back up with the world.
Component ids and type ids come from type names, so the host and the library always agree on them.

What doesn't survive a reload: components that aren't savable (rebuild them in `init`, as the sprites sample does
for its animators), state kept only in the `Game` object, and entity ids (references between saved entities are
remapped). See [guides/hot-reload.md](guides/hot-reload.md).

## Rendering

The renderer is built on SDL_GPU: Metal on macOS and iOS, Vulkan on Linux, Android and Windows, and Direct3D 12 on
Windows. `--driver vulkan|metal|direct3d12` picks one explicitly.

- **Sprites** use vertex pulling. Each sprite is a 64-byte record in a storage buffer, and the vertex shader builds
  the quad from `gl_VertexIndex`, so there are no vertex buffers. All sprites of a frame go to the GPU in one upload,
  with one draw call per run of sprites sharing a texture. Batches sort by depth, then texture, unless told otherwise.
- **Blending** uses premultiplied alpha throughout. Textures are premultiplied on load. Each batch picks a
  `BlendMode`: alpha, additive, multiply or opaque.
- **Materials:** a batch can use its own fragment shader (`gfx::Material`), with up to three extra textures and a
  256-byte uniform block. Pipelines are created on first use and cached per shader pair, target format and blend
  mode. A hot-reloaded shader gets a new pipeline.

A frame flows through these passes:

```
submit(batch)        -> scene (RGBA16F, HDR: sprite colors above 1 are allowed and glow with bloom)
submit_light(batch)  -> light map (half resolution, additive)                        [post().lighting]
                        bloom: bright pass, 6-level downsample + tent upsample chain    [post().bloom]
final pass           -> scene * (ambient + light) + bloom, exposure, tonemap, saturation, contrast, tint, vignette
submit_ui(batch)     -> drawn on the final image, untouched by lighting and post-processing
dev overlay          -> on top; --screenshot reads back this image; then it's copied to the window
```

With default `PostSettings` the final pass is an exact copy, so a game only pays for the effects it turns on.
[guides/rendering.md](guides/rendering.md) covers all of it.

## Shaders

Shaders are written once, in Vulkan-flavored GLSL, as `name.vert.glsl` and `name.frag.glsl`; `#include` works.
`ph-shaderc` compiles each into a `.phsh` bundle holding SPIR-V, MSL and HLSL (Shader Model 5.1) plus the resource
counts SDL_GPU needs. It remaps Metal slots and HLSL registers to SDL's layout, and when a resource is in the wrong
descriptor set it says which set to use. On Windows the D3D12 backend compiles the HLSL to DXBC at load time.

SDL_GPU's descriptor set layout:

| Stage | Textures, storage textures, storage buffers | Uniform buffers |
| --- | --- | --- |
| Vertex | set 0 | set 1 |
| Fragment | set 2 | set 3 |

The build precompiles every shader. Dev builds also link the compiler, so `.glsl` files compile when loaded and again
every time they're saved. See [guides/shaders.md](guides/shaders.md).

## Assets

`AssetManager` maps virtual paths to mounted folders, checking earlier mounts first. Dev builds mount the source
folders before the build output, so edits hot-reload. `Asset<T>` handles are shared: a reload swaps the value in place
and bumps `version()`, and a failed reload keeps the old value. Loaders read files through `AssetLoadContext`, which
records each file touched (shader `#include`s and a map's tilesets included) as a reason to reload. Assets made in code
go through `assets.add(path, value)`.

| Type | Files | Notes |
| --- | --- | --- |
| `gfx::Texture` | .png .jpg .tga .bmp | premultiplied on load; mipmapped unless the game is pixel art |
| `gfx::Shader` | .vert/.frag (.glsl or .phsh) | load by path without the extension |
| `gfx::Font` | .ttf .otf | glyphs rasterized on demand |
| `gfx::SpriteSheet` | Aseprite .json | loads its PNG too |
| `gfx::Tilemap` | Tiled .tmj (+ .tsj) | loads tileset and image-layer PNGs too |
| `audio::Sound` | .wav .ogg | fully decoded, for effects |
| `audio::Music` | .ogg | streamed |
| `StringTable` | lang/*.json | localization |

At runtime, `ph_add_game(target ASSETS dir)` lays out next to the executable:

```
<exe dir>/engine/   engine assets and compiled engine shaders
<exe dir>/assets/   game assets and compiled game shaders
```

See [guides/assets.md](guides/assets.md).

## Threading rules

- The main thread owns the `Engine` services, the `AssetManager`, the renderer and structural changes to the world.
- Systems run on worker threads. Inside `par_each`, touch only the current entity's components plus read-only data.
  Reading resources is fine if the system declares it (`reads_resource<T>()`); writing one makes the system
  conflict with every other user of that resource.
- `AudioEngine` is thread-safe; mixing happens on SDL's audio thread.
- Logging and the profiler are thread-safe.

## Design principles

These explain most of the choices in the code:

- **Code first.** A game is C++ code using plain types. Files (maps, sprite sheets, string tables) come from the
  editors artists already use: Tiled, Aseprite, any text editor.
- **Plain data.** Components, settings, particle settings and tweens are structs you can copy, save and inspect.
- **Fast iteration.** Anything that can be reloaded while the game runs is: assets, shaders, maps, strings, game code.
  A reload that fails keeps the last good version and logs why.
- **Predictable cost.** No hidden allocations in hot loops, one GPU upload of sprite data per frame, effects that cost
  nothing until they're turned on.
- **Deterministic by default.** Fixed timestep, deterministic parallel scheduling, seeded random generators.
- **One code path per platform feature.** One renderer for three GPU APIs, one entry point for every OS.
