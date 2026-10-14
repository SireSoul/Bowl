# PLACEHOLDERENGINENAME

A code-first 2D game engine in modern C++20 on SDL3. One GPU renderer for Vulkan, Metal and Direct3D 12, a parallel
archetype ECS, hot reload of assets and shaders, and in-game dev tools.

> The name is a placeholder. It appears only in `CMakeLists.txt` (`project(...)`), this README, and a few log/window
> strings; code uses the namespace `ph`.

## Status

Early but real: everything below builds, is tested, and runs. Samples are verified rendering on macOS (Metal) and
Windows (Direct3D 12, in CI). CI builds and tests on Windows, Linux and macOS. See [docs/roadmap.md](docs/roadmap.md)
for what's next.

| Module | What it does |
| --- | --- |
| `core` | types, math, easing + tweens, logging, assert, job system, profiler, file watcher, asset manager with hot reload, JSON, UTF-8, reflection |
| `ecs` | archetype ECS: plain-struct components, cached queries, `par_each`, command buffers, hooks, resources, parallel scheduler, reflection + JSON save/load + prefabs, parent/child transforms |
| `platform` | window, keyboard/mouse/gamepad, named actions with fixed-timestep-safe edges |
| `gfx` | SDL_GPU device, textures (mipmapped), shaders, instanced sprite batching (one upload, one draw per texture), cameras, TTF text (UTF-8, kerning, wrap, align), Aseprite sprite sheets + animation, Tiled maps (orthogonal + isometric, y-sorted), HDR + bloom + color grading + vignette, 2D lighting, particles, custom materials and blend modes, screenshots |
| `audio` | software mixer: 128 voices, buses, pitch, equal-power pan, looping; WAV and OGG sounds; streamed OGG music with gapless loops and crossfades |
| `ui` | immediate-mode game UI: panels, buttons, checkboxes, sliders, progress bars, nine-slice art, mouse + keyboard + gamepad focus, typewriter dialogue box |
| `physics` | collision: rect/circle overlap + separation, swept boxes (no tunneling), raycasts, spatial hash, `move_and_slide` |
| `app` | `Engine` + `Game`, scene stack with fades, localization, fixed-timestep loop with interpolation, SDL3 callback entry point, Dear ImGui dev overlay |
| `tools/shaderc` | GLSL → SPIR-V + Metal compiler (library + CLI), also linked into dev builds for live shader edits |

## Build

Needs CMake 3.25+ and a C++20 compiler (MSVC 2022, Clang 16+, GCC 13+). Every other dependency is downloaded at a
pinned version with a checked SHA-256. An installed SDL3 is used if present; otherwise it's built from source.

```bash
cmake --preset dev
```

```bash
cmake --build --preset dev
```

```bash
ctest --preset dev
```

```bash
./build/dev/samples/sprites/sprites
```

```bash
./build/dev/samples/isometric/isometric
```

Samples: `sprites` (ECS stress test, text HUD, animation, audio) and `isometric` (a y-sorted isometric map you walk
around).

Presets: `dev` (optimized + debug info + dev tools, the day-to-day build), `debug`, `release` (shipping: no dev tools,
precompiled shaders, SDL linked statically, `.app` bundles on macOS).

To ship, build `release` and install it into a folder. Every game lands there as a self-contained `.app` (macOS) or
an executable with its `engine/` and `assets/` folders (Windows, Linux):

```bash
cmake --install build/release --prefix dist
```

Command-line flags for any game: `--frames N` (quit after N frames), `--screenshot out.png` (what's on screen, overlay
included), `--overlay` (open the dev overlay at startup), `--no-vsync`, `--driver vulkan|metal|direct3d12`.

## A game in one file

```cpp
#include "ph/app/main.hpp"
using namespace ph;

struct Position { Vec2 value; };
struct Velocity { Vec2 value; };
PH_REFLECT(Position, value) // optional: savable + editable in the F1 inspector
PH_REFLECT(Velocity, value)

class MyGame : public Game {
    gfx::SpriteBatch batch;
    Asset<gfx::Texture> hero;

    void init(Engine& engine) override {
        hero = engine.assets().load<gfx::Texture>("textures/hero.png");   // hot-reloads when the file changes
        engine.input().bind("jump", {InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
        engine.world().create(Position{{0, 0}}, Velocity{{30, 0}});
        engine.fixed_schedule().par_each<Position, const Velocity>("move",
            [](ecs::SystemContext& ctx, ecs::Entity, Position& p, const Velocity& v) { p.value += v.value * ctx.dt; });
    }
    void render(Engine& engine) override {
        batch.begin(engine.camera());
        engine.world().each<const Position>([&](const Position& p) { batch.draw(*hero, p.value); });
        engine.renderer().submit(batch);
    }
};

PH_MAIN(MyGame)
```

```cmake
add_executable(my_game main.cpp)
ph_add_game(my_game ASSETS assets)
```

## Dev workflow

- **F1** opens the overlay: FPS graph, profiler zones, every system with its time and parallel batch (toggle systems
  on/off), entity/archetype browser, asset list with reload buttons, log, and pause / single-step / time-scale.
- Click an entity to inspect it: reflected components are edited live, and entities can be duplicated, destroyed or
  copied as JSON. The whole world saves and loads from the overlay. `--inspect N` opens entity N at startup.
- Dev builds read assets straight from your source folders. Save a PNG, WAV or `.glsl` file and the running game
  updates. A broken shader logs its error and keeps the last good version.
- `Game::dev_ui` is where your own ImGui tool windows go.

## Docs

- [docs/architecture.md](docs/architecture.md): how the pieces fit, and the rules (threading, determinism, assets)
- [docs/roadmap.md](docs/roadmap.md): what's next
