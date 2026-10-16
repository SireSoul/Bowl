# The game loop

A game is a class derived from `bowl::Game`, registered with `BOWL_MAIN`. The engine owns the loop and calls the game
back at fixed points. This page covers those callbacks, the fixed timestep, `AppConfig`, and the `Engine` object
every callback receives.

```cpp
#include "bowl/app/main.hpp"

class MyGame final : public bowl::Game {
    bowl::AppConfig configure() override;
    void init(bowl::Engine&) override;
    void start(bowl::Engine&) override;
    void fixed_update(bowl::Engine&, bowl::f32 dt) override;
    void update(bowl::Engine&, bowl::f32 dt) override;
    void render(bowl::Engine&) override;
    void dev_ui(bowl::Engine&) override;
    void event(bowl::Engine&, const SDL_Event&) override;
    void shutdown(bowl::Engine&) override;
};

BOWL_MAIN(MyGame)
```

All callbacks are optional. `BOWL_MAIN` goes in exactly one `.cpp` file.

## The callbacks

| Callback | When | Put here |
| --- | --- | --- |
| `configure()` | First, before the window exists | Return an `AppConfig` |
| `init(engine)` | Once at startup, and again after each [game-code hot reload](hot-reload.md#game-code) | Register systems, bind input, load assets, set resources |
| `start(engine)` | Once, after the first `init` | Create the world: spawn entities, push the first scene |
| `fixed_update(engine, dt)` | Every fixed tick (60 per second by default), after the fixed schedule | Gameplay and simulation |
| `update(engine, dt)` | Once per frame, after the frame schedule | Camera, animation, UI logic, anything visual |
| `render(engine)` | Once per frame | Fill sprite batches and submit them |
| `dev_ui(engine)` | Once per frame while the F1 overlay is open (dev builds) | Your own Dear ImGui windows |
| `event(engine, event)` | For each SDL event | Rarely needed: input goes through `engine.input()` |
| `shutdown(engine)` | Before the engine shuts down | Final cleanup |

The split between `init` and `start` matters for hot reload: `init` may run many times in one session, so it should
only set things up, never create entities. `start` runs once.

## Fixed timestep

Simulation runs at a fixed rate (`AppConfig::fixed_hz`, 60 by default), independent of the frame rate. Each frame, the
engine works out how much real time has passed and runs as many fixed ticks as fit. A 30 Hz frame rate runs two ticks
per frame; a 144 Hz screen runs a tick only on some frames.

Fixed ticks make gameplay behave the same on every machine and make simulations reproducible. Put anything that
affects game state in `fixed_update` or in systems on `engine.fixed_schedule()`.

If a frame is so slow that more than `max_fixed_steps` ticks are owed (8 by default), the rest of the owed time is
dropped. The game slows down briefly instead of getting further and further behind.

### Smooth drawing with alpha

Because frames and ticks don't line up, drawing positions straight from the simulation looks jittery. Keep the
previous tick's value and interpolate:

```cpp
struct Position {
    bowl::Vec2 value;
    bowl::Vec2 previous;
};

// a system at the start of the fixed schedule
fixed.par_each<Position>("remember position", [](Position& p) { p.previous = p.value; });

// when drawing
bowl::Vec2 at = bowl::lerp(p.previous, p.value, engine.alpha());
```

`engine.alpha()` is 0 right after a tick and approaches 1 just before the next.

### Input in fixed ticks

`input.pressed(...)` and `input.released(...)` mean "since the previous fixed tick" inside fixed updates and "since the
previous frame" elsewhere. A tap is never lost when a frame runs no ticks, and never seen twice when a frame runs
several. See [input.md](input.md).

## AppConfig

```cpp
bowl::AppConfig configure() override {
    bowl::AppConfig config;
    config.window.title = "My Game";
    config.window.width = 1600;
    config.window.height = 900;
    config.pixel_art = true;
    return config;
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `window.title` | `"Bowl"` | Window title, and the app name for the settings folder |
| `window.width`, `window.height` | 1280 x 720 | Initial size in logical points |
| `window.resizable` | `true` | |
| `window.fullscreen` | `false` | |
| `fixed_hz` | 60 | Fixed ticks per second |
| `max_fixed_steps` | 8 | Most fixed ticks in one frame |
| `vsync` | `true` | Also `--no-vsync`, and a checkbox in the overlay |
| `worker_threads` | -1 | Job system workers; -1 means one per hardware thread minus the main thread |
| `pixel_art` | `false` | Nearest filtering for textures and a pixel-snapped camera |
| `gpu_driver` | `""` | Force `"vulkan"`, `"metal"` or `"direct3d12"`; also `--driver` |
| `log_level` | `Info` | Lowest level logged |
| `language`, `fallback_language` | `"en"` | Localization; also `--lang` |
| `organization`, `application` | `"Bowl"`, title | Where settings and saves go (see below) |

## The Engine

Every callback receives `bowl::Engine&`, which owns every engine service:

| Accessor | What |
| --- | --- |
| `window()` | The window: `size()` in logical points, `pixel_size()`, `pixel_density()`, fullscreen |
| `gpu()` | The GPU device, for making textures in code |
| `renderer()` | Submitting batches, post-processing, clear color, stats |
| `camera()` | The main world camera; its viewport follows the window |
| `input()` | Keyboard, mouse, gamepads and actions |
| `audio()` | Playing sounds and music |
| `assets()` | Loading assets |
| `i18n()` | Localized strings |
| `jobs()` | The job system |
| `world()` | The ECS world |
| `fixed_schedule()`, `frame_schedule()` | Systems run every fixed tick / every frame |
| `scenes()` | The scene stack |
| `settings()` | Player preferences, saved at exit |
| `user_data_path()` | A writable per-user folder for settings and saves |
| `args()`, `has_arg(flag)` | Command-line arguments |

Time:

| | |
| --- | --- |
| `time()` | Simulated seconds: fixed ticks times the tick length |
| `delta()` | This frame's duration in seconds, scaled by the time scale, 0 while paused |
| `fixed_delta()` | Length of one fixed tick |
| `alpha()` | Position between the last two ticks, 0..1 |
| `frame()`, `tick()` | Frames drawn and fixed ticks run so far |

Simulation controls, also in the overlay: `set_paused(bool)`, `step()` (one tick while paused),
`set_time_scale(f32)`. `quit()` ends the game after the current frame.

## Where files go

`settings()` and `user_data_path()` use the operating system's per-user application data folder, named after
`organization` and `application`:

| OS | Folder |
| --- | --- |
| Windows | `%APPDATA%\<organization>\<application>\` |
| macOS | `~/Library/Application Support/<organization>/<application>/` |
| Linux | `~/.local/share/<organization>/<application>/` (or `$XDG_DATA_HOME`) |

Set `organization` to your studio's name before you ship; changing it later moves where players' settings are read
from.
