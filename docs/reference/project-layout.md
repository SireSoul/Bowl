# Project layout

## The repository

```
CMakeLists.txt            top-level build: options, modules, tests, samples
CMakePresets.json         dev, debug, release
.clang-format             code formatting
.github/                  CI workflow, issue and pull request templates
cmake/
  PhCompiler.cmake        ph_configure_target, ph_optimize_debug: flags and warnings
  PhDependencies.cmake    third-party libraries, pinned by URL and SHA-256
  PhShaders.cmake         ph_compile_shaders, ph_add_game, game-code hot reload
engine/
  CMakeLists.txt          one static library per module, and ph::engine
  assets/                 engine assets, mounted as "engine/"
    fonts/                Roboto Medium and its license
    shaders/              sprite, full-screen and post-processing shaders, common.glsl
  core/                   no SDL: the foundation everything else builds on
  ecs/                    entities, components, queries, schedules, world I/O
  platform/               window and input
  gfx/                    GPU device, sprites, renderer, text, sprite sheets, tilemaps, particles
  audio/                  mixer, sounds, music
  physics/                collision
  ui/                     game UI
  net/                    UDP connections, lockstep, rollback
  app/                    Engine, Game, scenes, the main loop, dev tools, hot reload host
tools/
  shaderc/                ph-shaderc: library and command-line tool
  atlas/                  ph-atlas: packs pictures into an atlas with sprite-sheet JSON
editor/                   Bowl Editor
samples/
  tutorial/               Fireflies, the game docs/tutorial.md builds
  isometric/              y-sorted isometric map, night lighting, menu, dialogue
  sprites/                ECS stress test, custom shader, animation, hot-reloadable
tests/
  test_*.cpp              unit tests, one file per area
  bench_ecs.cpp           ECS benchmark (not a test)
  data/                   test fixtures (small OGG files) and how they were made
docs/                     this documentation; images/ holds the screenshots
```

Every module follows the same shape:

```
engine/<module>/
  include/ph/<module>/    public headers: #include "ph/<module>/<file>.hpp"
  src/                    implementation and private headers
```

## Headers by module

| Module | Header | What's in it |
| --- | --- | --- |
| core | `types.hpp` | `u8`..`u64`, `i8`..`i64`, `f32`, `f64`, `usize`, `NonCopyable` |
| | `math.hpp` | `Vec2`, `Vec3`, `Vec4`, `Mat4`, `Rect`, `Color`, `lerp`, `clamp`, `pi`, `tau` |
| | `ease.hpp` | Easing curves, `Tween<T>` |
| | `assert.hpp` | `PH_ASSERT`, `PH_VERIFY`, `PH_UNREACHABLE` |
| | `log.hpp` | `trace` .. `error`, sinks |
| | `jobs.hpp` | `JobSystem`, `TaskGroup` |
| | `profiler.hpp` | `PH_PROFILE_SCOPE`, frame stats |
| | `time.hpp` | `now_seconds`, `Stopwatch` |
| | `file.hpp` | Reading and writing files, `FileWatcher` |
| | `assets.hpp` | `AssetManager`, `Asset<T>`, `AssetLoadContext` |
| | `json.hpp` | `json::Value`, `parse`, `stringify` |
| | `xml.hpp` | `xml::Element`, `xml::parse` (enough XML for tool formats) |
| | `reflect.hpp` | `PH_REFLECT`, field visitors |
| | `serialize.hpp` | `serial::to_json`, `serial::from_json`, `serial::Context` |
| | `settings.hpp` | `Settings` |
| | `localization.hpp` | `Localization`, `StringTable` |
| | `utf8.hpp` | UTF-8 decoding and encoding |
| | `type_id.hpp` | `type_id<T>()`, `type_name<T>()`, `fnv1a` |
| | `function_ref.hpp` | `FunctionRef`, a non-owning callable reference |
| | `shader_bundle.hpp` | The `.phsh` format |
| ecs | `ecs.hpp` | Includes the rest |
| | `component.hpp` | `Entity`, component registration, `PH_COMPONENT` |
| | `world.hpp` | `World`, `Query`, `QueryFilter` |
| | `commands.hpp` | `CommandBuffer` |
| | `schedule.hpp` | `Schedule`, `SystemContext` |
| | `hierarchy.hpp` | `Transform2D`, `GlobalTransform2D`, parenting |
| | `world_io.hpp` | `save_world`, `load_entities`, `spawn_prefab` |
| | `archetype.hpp` | Storage (internal, but public for tools) |
| platform | `window.hpp` | `Window`, `WindowDesc` |
| | `input.hpp` | `Input`, `Key`, `MouseButton`, gamepad enums, `InputBinding` |
| gfx | `device.hpp` | `GpuDevice`, `Texture`, `Shader`, `TextureDesc` |
| | `sprites.hpp` | `SpriteBatch`, `StaticSprites`, `SpriteInstance`, `Camera2D`, `Material`, `BlendMode`, `SpriteSort` |
| | `renderer.hpp` | `Renderer`, `PostSettings`, `write_png` |
| | `text.hpp` | `Font`, `draw_text`, `measure_text`, `TextStyle` |
| | `sprite_sheet.hpp` | `SpriteSheet`, `SpriteAnimator`, `draw_frame` |
| | `tilemap.hpp` | `Tilemap`, `draw_tilemap`, `TilemapCache` |
| | `particles.hpp` | `ParticleSystem`, `ParticleEmitter` |
| audio | `audio.hpp` | `AudioEngine`, `Sound`, `Music`, buses |
| physics | `collision.hpp` | Shapes, `collide`, `sweep`, `raycast`, `move_and_slide`, `SpatialHash` |
| ui | `ui.hpp` | `ui::Context`, `ui::Style`, `ui::Typewriter` |
| app | `main.hpp` | `PH_MAIN`; include it in one file |
| | `engine.hpp` | `Engine`, `Game`, `AppConfig` |
| | `scene.hpp` | `Scene`, `SceneStack` |
| | `hot_reload.hpp` | `HotReloadHost` (used by the hot reload build; games don't need it) |

`ph/app/main.hpp` brings in the engine, the whole ECS (`ecs.hpp`, including the hierarchy and world I/O), the renderer,
audio, input, assets, settings, localization and logging. Include these yourself where you use them: `text.hpp`,
`sprite_sheet.hpp`, `tilemap.hpp`, `particles.hpp`, `collision.hpp`, `ui.hpp` and `ease.hpp`.

## A game project

A suggested layout for a game outside this repository:

```
my_game/
  CMakeLists.txt
  CMakePresets.json        copied from the engine and adjusted
  external/
    Bowl/ the engine, as a Git submodule
  src/
    main.cpp               PH_MAIN and the Game class
    components.hpp         PH_COMPONENT structs
    systems/               one file per group of systems
    scenes/                one file per scene
  assets/
    characters/            Aseprite sheets (.json + .png)
    maps/                  Tiled maps (.tmj, .tsj) and tilesets
    music/  sfx/           .ogg, .wav
    shaders/               *.vert.glsl, *.frag.glsl
    fonts/
    lang/                  en.json, es.json, ...
```

## A built game

After building, each game's folder looks like this (`build/<preset>/<path to game>/`, plus `RelWithDebInfo/` or
`Release/` on Windows):

```
my_game(.exe)
engine/                    engine assets and compiled engine shaders (.phsh)
assets/                    copies of your assets, plus compiled shaders (.phsh)
my_game_shaders/           (next to it, in the build tree) intermediate shader outputs
```

In dev builds the game reads from your source folders first, so the copies matter only for release builds and
installs. [packaging.md](../guides/packaging.md) shows the installed layout.

## Where the game writes

| | |
| --- | --- |
| Player settings | `<user data>/settings.json` (see [game-loop.md](../guides/game-loop.md#where-files-go)) |
| Your save files | wherever you choose; `engine.user_data_path()` is the place for them |
| Overlay layout | `devtools.ini` in the working directory (dev builds) |
| Overlay world save | `devtools_world.json` in the working directory (dev builds) |
