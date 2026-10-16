# app

Made by `ph-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target ph_api_docs`.

- [ph/app/engine.hpp](#phappenginehpp)
- [ph/app/entry.hpp](#phappentryhpp)
- [ph/app/hot_reload.hpp](#phapphot_reloadhpp)
- [ph/app/icon.hpp](#phappiconhpp)
- [ph/app/inspector.hpp](#phappinspectorhpp)
- [ph/app/main.hpp](#phappmainhpp)
- [ph/app/scene.hpp](#phappscenehpp)

## ph/app/engine.hpp

`#include "ph/app/engine.hpp"` · namespace `ph`

```cpp
union SDL_Event;
```

### AppConfig

```cpp
struct AppConfig {
    WindowDesc window;
    f64 fixed_hz = 60;               // simulation rate of fixed_update and the fixed schedule
    i32 max_fixed_steps = 8;         // per frame, so a slow frame can't spiral
    bool vsync = true;
    i32 worker_threads = -1;         // -1 = one per hardware thread, minus the main thread
    bool pixel_art = false;          // nearest-filtered textures and a pixel-snapped camera
    std::string gpu_driver;          // "", "vulkan", "metal", "direct3d12"
    LogLevel log_level = LogLevel::Info;
    std::string language = "en";     // lang/<language>.json, falling back to fallback_language
    std::string fallback_language = "en";
    // Where settings and saves live: the OS's per-user app data folder for <organization>/<application>.
    std::string organization = "Bowl";
    std::string application;         // empty: the window title
    std::string game_asset_dir;      // dev builds: the game's source asset folder (set by PH_MAIN)
    bool fullscreen_on_steam_deck = true; // see is_steam_deck() in ph/platform/system.hpp
    // For tools built on the engine, like the editor: Game::dev_ui is always shown, not only while the F1 overlay
    // is open, and the engine's own windows move from F1 to F12. Dev builds only.
    bool dev_ui_always = false;
};
```

### Game

Implement this, then put PH_MAIN(YourGame) in one .cpp file (see ph/app/main.hpp).

```cpp
class Game
```

```cpp
virtual ~Game() = default;
```

Called first, before the window exists.

```cpp
virtual AppConfig configure();
```

Setup that code changes may need redone: register systems, bind inputs, load assets, set resources.
Runs at startup, and again after each game-code hot reload.

```cpp
virtual void init(Engine&);
```

Create the world (spawn entities). Runs once, after the first init; a hot reload keeps the world instead.

```cpp
virtual void start(Engine&);
```

Simulation at a fixed rate (after the fixed schedule). Deterministic code goes here.

```cpp
virtual void fixed_update(Engine&, f32 /*dt*/);
```

Once per frame (after the frame schedule): animation, camera, UI.

```cpp
virtual void update(Engine&, f32 /*dt*/);
```

Fill sprite batches and engine.renderer().submit() them.

```cpp
virtual void render(Engine&);
```

Dev builds: draw your own Dear ImGui windows here; shown with the overlay (F1).

```cpp
virtual void dev_ui(Engine&);
virtual void event(Engine&, const SDL_Event&);
virtual void shutdown(Engine&);
```

### Engine

Every engine service, owned in one place and handed to the game.

```cpp
class Engine : NonCopyable
```

```cpp
Engine();
~Engine();
```

```cpp
Window& window();
gfx::GpuDevice& gpu();
gfx::Renderer& renderer();
Input& input();
audio::AudioEngine& audio();
AssetManager& assets();
```

Strings for the current language, from assets lang/&lt;code>.json (see AppConfig::language).

```cpp
Localization& i18n();
JobSystem& jobs();
ecs::World& world();
```

Runs every fixed tick, before Game::fixed_update.

```cpp
ecs::Schedule& fixed_schedule();
```

Runs every frame, before Game::update.

```cpp
ecs::Schedule& frame_schedule();
```

The main world camera; its viewport follows the window.

```cpp
gfx::Camera2D& camera();
```

Screens such as the title, gameplay and menus. They run after the Game's own callbacks each frame.

```cpp
SceneStack& scenes();
```

Player preferences, saved to user_data_path()/settings.json at exit. Controls saved under "input" (with
save_controls) are applied after Game::init, so player rebinds override the game's defaults.

```cpp
Settings& settings();
void save_controls();
```

A writable per-user folder for settings and save games (created on first use).

```cpp
const std::filesystem::path& user_data_path() const;
const AppConfig& config() const;
```

Command-line arguments (argv[1..]), including the engine's own flags.

```cpp
const std::vector<std::string>& args() const;
bool has_arg(std::string_view flag) const;
```

```cpp
f64 time() const; // simulated seconds (fixed ticks * fixed_dt)
f32 delta() const; // this frame, scaled
f32 fixed_delta() const;
```

How far between the last two fixed ticks this frame is (0..1), for smooth rendering of fixed-rate motion.

```cpp
f32 alpha() const;
u64 frame() const;
u64 tick() const;
```

Simulation controls (also on the dev overlay).

```cpp
void set_paused(bool paused);
bool paused() const;
void step();
void set_time_scale(f32 scale);
f32 time_scale() const;
```

```cpp
void quit();
```

In the background (phones and tablets): nothing updates or draws until the game comes back. Save the game when
SDL_EVENT_WILL_ENTER_BACKGROUND reaches Game::event, since the system may close it from there without warning.

```cpp
bool suspended() const;
```

## ph/app/entry.hpp

`#include "ph/app/entry.hpp"`

The engine side of the entry point. Games include ph/app/main.hpp instead.

## ph/app/hot_reload.hpp

`#include "ph/app/hot_reload.hpp"` · namespace `ph`

### HotReloadHost

Game-code hot reload (dev builds). The game is built as a loadable library and run by this host: when the library
file changes (you rebuilt it), the host
  1. keeps the world: entities, their ids, and every component whose code isn't in the game library. Of the game's
```cpp
 own components, plain data (trivially copyable) stays in place, savable ones are saved to come back through
 the new code, and any others are dropped,
```
  2. releases everything else made by the old code: systems, scenes, input bindings, resources, hooks, and the
```cpp
 assets and loaders registered since startup,
```
  3. unloads the old library, loads the new one (if that fails, the old code keeps running), creates the new Game,
```cpp
 runs its init(), and puts the saved components back.
```
A plain-data component whose size or alignment changed loses its old data. Use PH_COMPONENT for savable components
so they're registered as soon as the library loads. State kept only in the Game object is rebuilt by init().

With source_dirs and build_command set (ph_add_game does this), the host also rebuilds the library itself when a
source file is saved, so editing code is enough. --no-auto-build turns that off.

```cpp
class HotReloadHost : public Game
```

#### Options

```cpp
struct Options {
    std::filesystem::path library;
    std::vector<std::filesystem::path> source_dirs; // watched recursively for C and C++ files
    std::vector<std::string> build_command;         // run when they change; empty: rebuild by hand
};
```

```cpp
explicit HotReloadHost(Options options);
~HotReloadHost() override;
```

```cpp
AppConfig configure() override;
void init(Engine& engine) override;
void start(Engine& engine) override;
void fixed_update(Engine& engine, f32 dt) override;
void update(Engine& engine, f32 dt) override;
void render(Engine& engine) override;
void dev_ui(Engine& engine) override;
void event(Engine& engine, const SDL_Event& e) override;
void shutdown(Engine& engine) override;
```

```cpp
u32 reloads() const;
```

### AutoBuilder

Watches source folders and runs a build command when a source file changes, on a background thread. Used by
HotReloadHost; public so tools can reuse it.

```cpp
class AutoBuilder : NonCopyable
```

```cpp
AutoBuilder(std::vector<std::filesystem::path> source_dirs, std::vector<std::string> command);
~AutoBuilder(); // waits for a running build
```

Call regularly from the main thread: notices changes (a few times a second), starts builds once files have
been quiet for a moment, and logs the outcome of finished builds.

```cpp
void poll();
bool building() const;
usize watched_files() const;
```

Whether a path is a C or C++ source or header, by extension.

```cpp
static bool is_source(const std::filesystem::path& path);
```

## ph/app/icon.hpp

`#include "ph/app/icon.hpp"` · namespace `ph`

The game's icon as a PNG, put into the executable by ph_add_game(... ICON icon.png); empty without one. The App
shows it on the window (title bar, taskbar, and the Dock outside an .app bundle, which has its own .icns).

```cpp
std::span<const u8> embedded_icon();
```

### EmbeddedIcon

What the file ph_add_game makes uses to register the icon.

```cpp
struct EmbeddedIcon
```

```cpp
EmbeddedIcon(const u8* png, usize size);
```

## ph/app/inspector.hpp

`#include "ph/app/inspector.hpp"` · namespace `ph::inspector`

Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). Asset fields accept it.

```cpp
inline constexpr const char* asset_payload = "PH_ASSET_PATH";
```

One component of an entity under a collapsing header: editable fields for reflected components (lists included), a
note for the rest, and a close button that removes the component. Clicking an entity field stores that entity in
*select. Returns true if anything changed.

```cpp
bool component(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select);
```

Every component of the entity, in order, then add_component_menu.

```cpp
bool components(ecs::World& world, ecs::Entity e, AssetManager& assets, ecs::Entity* select);
```

An "Add component" button: a searchable list of the savable component types the entity doesn't have yet.

```cpp
bool add_component_menu(ecs::World& world, ecs::Entity e);
```

Whether an entity matches a filter (these two work in every build): every word must appear, ignoring case, in one
of its component names or its Name, or be "#index". "player health" finds entities with both; "#42" finds entity 42.

```cpp
bool matches(const ecs::World& world, ecs::Entity e, std::string_view filter);
bool text_matches(std::string_view text, std::string_view filter);
```

Just the editable fields of one component, without a header (for tools that draw their own). Returns true if a
value changed; false (and nothing drawn) for components that aren't reflected.

```cpp
bool fields(ecs::World& world, ecs::Entity e, ecs::ComponentId c, AssetManager& assets, ecs::Entity* select);
```

## ph/app/main.hpp

`#include "ph/app/main.hpp"`

Include in exactly one .cpp of your game and write PH_MAIN(YourGame).

Uses SDL3's callback entry points instead of a main() loop. The web (Emscripten), iOS and Android need those, and
using them everywhere keeps the game code identical on every platform. In a hot-reload build (ph_add_game(... HOT_RELOAD)) the game is a
library instead, and PH_MAIN exports the function the host uses to create the game.

```cpp
#define PH_DETAIL_EXPORT extern "C" __declspec(dllexport)
#define PH_DETAIL_EXPORT extern "C" __attribute__((visibility("default")))
#define PH_MAIN(GameType)
```

```cpp
#define SDL_MAIN_USE_CALLBACKS 1
```

```cpp
#define PH_DETAIL_GAME_ASSETS PH_GAME_ASSET_SOURCE_DIR
#define PH_DETAIL_GAME_ASSETS ""
```

```cpp
#define PH_MAIN(GameType)
```

## ph/app/scene.hpp

`#include "ph/app/scene.hpp"` · namespace `ph`

### Scene

One screen of a game: title, gameplay, inventory, pause menu... Scenes stack: the top one gets input and updates;
scenes below keep drawing if the ones above are transparent (a pause menu over the paused game).

```cpp
class Scene
```

```cpp
virtual ~Scene() = default;
virtual void enter(Engine&); // pushed or became the replacement
virtual void exit(Engine&); // popped or replaced
virtual void pause(Engine&); // another scene was pushed on top
virtual void resume(Engine&); // the scene on top was popped
virtual void fixed_update(Engine&, f32 /*dt*/);
virtual void update(Engine&, f32 /*dt*/);
virtual void render(Engine&);
virtual void dev_ui(Engine&);
```

Draw the scene below this one too (overlays, menus over gameplay).

```cpp
virtual bool transparent() const;
```

Keep updating the scene below while this one is on top (e.g. a HUD-like overlay over live gameplay).

```cpp
virtual bool passes_updates() const;
```

### SceneStack

The engine owns one (engine.scenes()) and runs it after the Game's own callbacks, so a game can be all scenes:

```cpp
void init(Engine& e) override { e.scenes().push(std::make_unique<TitleScene>()); }
...
engine.scenes().replace(std::make_unique<GameplayScene>(), 0.4f); // fade out, swap, fade in
```
Changes requested during a frame happen at the end of it, so a scene can safely replace itself.

```cpp
class SceneStack : NonCopyable
```

```cpp
void push(std::unique_ptr<Scene> scene, f32 fade = 0);
void pop(f32 fade = 0);
void replace(std::unique_ptr<Scene> scene, f32 fade = 0);
void clear();
```

```cpp
Scene* top() const;
usize size() const;
bool empty() const;
bool transitioning() const;
Color fade_color = Color::black();
```

Driven by the App.

```cpp
void fixed_update(Engine& engine, f32 dt);
void update(Engine& engine, f32 dt);
void render(Engine& engine);
void dev_ui(Engine& engine);
void apply_pending(Engine& engine);
void shutdown(Engine& engine);
```

