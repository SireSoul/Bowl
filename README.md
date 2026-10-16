# Bowl

A code-first 2D game engine in C++20, built on SDL3. One renderer for Vulkan, Metal and Direct3D 12, a parallel
archetype ECS, hot reload for assets, shaders and game code, and dev tools that run inside the game.

<p align="center">
  <img src="docs/images/isometric-night.jpg" width="49%" alt="The isometric sample at night: lanterns on stone pillars, bloom and fireflies">
  <img src="docs/images/dev-overlay.png" width="49%" alt="The dev overlay: profiler, entity inspector and a custom tool window over the sprites sample">
</p>

You write the game in C++: a `Game` class with a few callbacks, components that are plain structs, systems that are
lambdas, assets loaded by path. The engine handles the loop, the GPU, audio, input and saving. In a dev build you can
change a texture, a shader, a Tiled map or the game's own code while it runs, and poke at any entity from the overlay.

It's built for 2D games with a lot going on, top-down and isometric included, that want to look good and save and
load their worlds without a pile of boilerplate.

## Features

**Core**
- Fixed-timestep simulation with interpolated rendering, so motion is smooth at any refresh rate
- Job system with deterministic chunking, a CPU profiler, logging, assertions
- Asset manager with virtual paths, mounts and hot reload; a failed reload keeps the last good version. Background
  loading (textures and sounds decode on the loading thread), and asset archives (`bowl-pack`) for release builds
- JSON (comments and trailing commas allowed in hand-written files) with a binary form for big files, a small XML
  reader, UTF-8, easing curves and tweens

**Entities**
- Archetype ECS: components are plain structs, queries are cached, `par_each` spreads work over every core
- A scheduler that runs non-conflicting systems in parallel and still gives the same result as running them in order,
  entity ids included
- Command buffers for structural changes, hooks, typed resources
- `BOWL_REFLECT` / `BOWL_COMPONENT` reflection: JSON save and load, entity references that survive a reload
- Prefabs and `.world` files as hot-reloading assets; binary saves, and save-file migrations for renamed or removed
  components and fields
- Parent/child transforms

**Graphics**
- SDL_GPU renderer: Vulkan, Metal and Direct3D 12 from one code path
- Instanced sprite batches: one upload per frame, one draw call per texture run, depth or texture sorting; static
  sprites uploaded once
- HDR scene, bloom, exposure, contrast, saturation, tint, ACES tonemapping, vignette, color-grading LUTs, chromatic
  aberration, film grain, and full-screen effects of your own
- 2D lighting: an additive light map, ambient color, emissive sprites, normal maps and shadows from occluders
- Custom materials: your own fragment shader per batch, extra textures, a uniform block, four blend modes
- Shaders, compute shaders included, written once in GLSL and compiled to SPIR-V, MSL and HLSL by `bowl-shaderc`
- Particles on the CPU, or simulated and drawn on the GPU
- TrueType text with kerning, wrapping, alignment, outlines, shadows, rich-text markup, fallback fonts, distance-field
  text and typewriter reveals, sharp at any size; right-to-left text, color emoji, and HarfBuzz shaping for Arabic,
  Devanagari and other complex scripts (optional)
- Aseprite sprite sheets, slices and animation events; `bowl-atlas` packs a folder of PNGs into an atlas
- Tiled maps (JSON and XML; orthogonal, isometric, staggered and hexagonal; y-sorted, animated tiles, object
  templates), with big maps kept on the GPU in chunks
- Screenshots from the command line

**Everything else**
- Audio: 128-voice mixer, buses with low-pass, high-pass and reverb, per-voice filters, ducking, pitch and pan,
  positional sounds; WAV, OGG and (optional) Opus sounds; streamed music with gapless loops and crossfades
- Input: keyboard, mouse, touch and up to four gamepads, named actions, rebinding saved to the player's settings
- Game UI: panels, buttons, checkboxes, sliders, progress bars, nine-slice art, dialogue boxes; mouse, keyboard and
  gamepad navigation
- Scene stack with fade transitions
- Collision: rectangles, circles, capsules and convex polygons, swept boxes that don't tunnel, raycasts, a spatial
  hash, `move_and_slide`, colliders and trigger events as components
- Optional Box2D v3 rigid bodies, with joints, springs, motors and contact events
- Networking: `net::Host` connections over UDP with reliable and unreliable messages and a simulated bad network;
  lockstep and rollback helpers built on the deterministic ECS
- Localization with fallbacks, placeholders and plurals
- Player settings stored in the OS's per-user folder

**Workflow**
- F1 dev overlay: frame graph, profiler, systems (timing, parallel batches, on/off), entity browser and inspector (add
  and remove components, edit lists), assets with reload buttons, log, pause, single-step, slow motion
- Game-code hot reload on Windows, macOS and Linux: save a source file, it rebuilds, and the new code takes over with
  the world kept
- Bowl Editor, in three workspaces: Level lays out levels with the game's own renderer (art shelf, groups, a brush,
  trigger regions and collision shapes, undo, the game's own components); Data edits every JSON file as a tree or as
  text, keeping its comments; Paint is a pixel-art canvas
- Tracy support for timeline profiling
- Packaging: release builds with static SDL, precompiled shaders and packed assets, `.app` bundles on macOS and
  `cmake --install` layouts; app icons from one PNG; signing on macOS (and notarization) and Windows; installers
  (DMG, NSIS, ZIP), a Linux AppImage and a Flatpak manifest; Steam Deck detection and a `--deck` preview window
- An API reference generated from the headers' comments by `bowl-apidoc`

## Quick start

You need CMake 3.25 or newer and a C++20 compiler: Visual Studio 2022, Xcode 15 (Apple Clang), Clang 16 or GCC 13.
All other dependencies are downloaded at pinned versions and checked against a SHA-256. An installed SDL3 is used if
there is one; otherwise SDL3 is built from source.

```bash
git clone https://github.com/SireSoul/Bowl.git
```

```bash
cd Bowl
```

```bash
cmake --preset dev
```

```bash
cmake --build --preset dev
```

```bash
ctest --preset dev
```

Then run a sample (on Windows the executables are one folder deeper, in `RelWithDebInfo`):

```bash
./build/dev/samples/isometric/isometric --night
```

[docs/getting-started.md](docs/getting-started.md) has the per-platform setup, IDE tips and what each preset does.

## A game in one file

```cpp
#include "bowl/app/main.hpp"
using namespace bowl;

struct Position { Vec2 value; };
struct Velocity { Vec2 value; };
BOWL_COMPONENT(Position, value) // optional: savable, editable in the F1 inspector, survives hot reload
BOWL_COMPONENT(Velocity, value)

class MyGame final : public Game {
    gfx::SpriteBatch batch_;
    Asset<gfx::Texture> hero_;

    void init(Engine& engine) override {
        hero_ = engine.assets().load<gfx::Texture>("textures/hero.png"); // reloads when the file changes
        engine.fixed_schedule().par_each<Position, const Velocity>(
            "move", [](ecs::SystemContext& ctx, ecs::Entity, Position& p, const Velocity& v) {
                p.value += v.value * ctx.dt;
            });
    }
    void start(Engine& engine) override {
        engine.world().create(Position{{100, 100}}, Velocity{{40, 0}});
    }
    void render(Engine& engine) override {
        if (!hero_) return; // file missing or broken: the log says why, and it loads once it's fixed
        batch_.begin(engine.camera());
        engine.world().each<const Position>([&](const Position& p) { batch_.draw(*hero_, p.value); });
        engine.renderer().submit(batch_);
    }
};

BOWL_MAIN(MyGame)
```

```cmake
add_executable(my_game main.cpp)
bowl_add_game(my_game ASSETS assets)
```

The [tutorial](docs/tutorial.md) builds a complete small game from here, one feature at a time.

## Samples

| | |
| --- | --- |
| <img src="docs/images/tutorial-play.png" width="400" alt="Fireflies"> | **`tutorial`**: Fireflies, the game the [tutorial](docs/tutorial.md) builds. Scenes, a title menu, ECS systems, collision, lighting, particles, sound and a saved high score. |
| <img src="docs/images/isometric-day.jpg" width="400" alt="Isometric sample"> | **`isometric`**: a y-sorted isometric map to walk around, with collision, a night mode (lights, bloom, fireflies, a torch whose light the pillars block), a pause menu and a dialogue box. Flags: `--night`, `--menu`, `--dialogue`. |
| <img src="docs/images/sprites.png" width="400" alt="Sprites sample"> | **`sprites`**: an ECS stress test with thousands of bouncing sprites, a custom shader, animated sprite sheets, audio and a custom dev tool window. Built with game-code hot reload. Flags: `--gpu-particles` (200,000 particles on the GPU), `--film` (a color-grading LUT, grain and chromatic aberration). |

## Documentation

Start at [docs/README.md](docs/README.md), the full index. The main entry points:

| | |
| --- | --- |
| [Getting started](docs/getting-started.md) | Toolchains for Windows, macOS and Linux; building, running, IDEs |
| [Tutorial](docs/tutorial.md) | Make a complete game, step by step |
| [Architecture](docs/architecture.md) | How the modules fit together, the frame, threading rules |
| [Guides](docs/README.md#guides) | One page per system: ECS, rendering, shaders, assets, text, sprite sheets, tilemaps, audio, input, UI, scenes, collision, saving, networking, hot reload, dev tools, the editor, packaging and more |
| [Reference](docs/README.md#reference) | The API (every public header), CMake options and functions, command-line flags, project layout, conventions |
| [Troubleshooting](docs/troubleshooting.md), [FAQ](docs/faq.md) | Common problems and questions |
| [Roadmap](docs/roadmap.md) | What's done and what's next |

## Platforms

| Platform | GPU backend | Status |
| --- | --- | --- |
| Windows 10 and 11 | Direct3D 12 (Vulkan also works) | Built and tested on CI; every CI run renders a sample on D3D12 through WARP |
| macOS | Metal | Built and tested on CI; the main development machine |
| Linux (X11, Wayland) | Vulkan | Built and tested on CI |
| iOS 17+, iPadOS 17+ | Metal | The `ios` preset builds the engine and samples for the simulator; not run on a simulator or device yet |
| Android | Vulkan | Not set up yet |
| Web | waiting on WebGPU support in SDL_GPU | Planned |

[docs/guides/platforms.md](docs/guides/platforms.md) has the details.

## Repository layout

```
engine/      the engine: core, ecs, platform, gfx, audio, physics, ui, net, app (+ engine assets)
editor/      Bowl Editor
tools/       bowl-shaderc (shaders), bowl-atlas (atlases), bowl-icon (app icons), bowl-pack (asset archives),
             bowl-apidoc (the API reference)
samples/     tutorial, isometric, sprites
tests/       unit tests (doctest) and an ECS benchmark
cmake/       build helpers: dependencies, compiler flags, shaders, bowl_add_game, packaging
docs/        the documentation
```

[docs/reference/project-layout.md](docs/reference/project-layout.md) goes through it file by file.

## Status

Early, but everything listed above builds, passes its tests and runs. The API will keep changing while the first
game is built on it; [CHANGELOG.md](CHANGELOG.md) records what changed.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers the workflow, code and comment style, and how to run the same checks as CI.

## License

Bowl is not open source: all rights are reserved ([LICENSE](LICENSE)). You're welcome to read the code, but not to
use, copy or redistribute it without permission.

Third-party code keeps its own license: SDL (zlib), stb (public domain or MIT), glslang (BSD-3-Clause and others),
SPIRV-Cross (Apache-2.0), Dear ImGui (MIT), doctest (MIT) and the Roboto font (Apache-2.0, in `engine/assets/fonts`).
Optional ones, fetched only when their option is on: Box2D (MIT), HarfBuzz (Old MIT), libogg, libopus and opusfile
(BSD-3-Clause) and Tracy (BSD-3-Clause).
