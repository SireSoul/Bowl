# app

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/app/dev_link.hpp](#bowlappdev_linkhpp)
- [bowl/app/engine.hpp](#bowlappenginehpp)
- [bowl/app/entry.hpp](#bowlappentryhpp)
- [bowl/app/hot_reload.hpp](#bowlapphot_reloadhpp)
- [bowl/app/icon.hpp](#bowlappiconhpp)
- [bowl/app/inspector.hpp](#bowlappinspectorhpp)
- [bowl/app/main.hpp](#bowlappmainhpp)
- [bowl/app/scene.hpp](#bowlappscenehpp)

## bowl/app/dev_link.hpp

`#include "bowl/app/dev_link.hpp"` · namespace `bowl::devlink`

```cpp
inline constexpr u32 protocol_version = 1;
```

### Hello

Game to editor.

```cpp
struct Hello {
    std::string game;
    u32 version = protocol_version;
};
```

```cpp
BOWL_REFLECT(Hello, game, version)
```

### Schedules

```cpp
struct Schedules {
    std::vector<ecs::SystemInfo> fixed, frame;
};
```

```cpp
BOWL_REFLECT(Schedules, fixed, frame)
```

### Zone

```cpp
struct Zone {
    std::string name;
    f64 ms = 0; // this frame, all calls and threads
    u32 calls = 0;
};
```

```cpp
BOWL_REFLECT(Zone, name, ms, calls)
```

### Frame

```cpp
struct Frame {
    f32 ms = 0; // CPU time of the last frame
    u32 entities = 0, archetypes = 0, voices = 0, sprites = 0, draw_calls = 0;
    bool paused = false;
    f32 time_scale = 1;
    std::vector<Zone> zones;
};
```

```cpp
BOWL_REFLECT(Frame, ms, entities, archetypes, voices, sprites, draw_calls, paused, time_scale, zones)
```

### EntityRow

```cpp
struct EntityRow {
    u32 index = 0, generation = 0; // the entity, in two halves (JSON numbers hold 53 bits)
    std::string name;              // its ecs::Name, if any
    std::vector<std::string> components;
};
```

```cpp
BOWL_REFLECT(EntityRow, index, generation, name, components)
```

### EntityPage

```cpp
struct EntityPage {
    u32 from = 0, total = 0; // total: entities matching the filter
    std::vector<EntityRow> rows;
};
```

```cpp
BOWL_REFLECT(EntityPage, from, total, rows)
```

### ComponentJson

```cpp
struct ComponentJson {
    std::string name;
    std::string json;      // as a save writes it; empty if it can't be shown
    bool editable = false; // savable, and small enough to show
};
```

```cpp
BOWL_REFLECT(ComponentJson, name, json, editable)
```

### EntityDetail

```cpp
struct EntityDetail {
    u32 index = 0, generation = 0;
    bool alive = false;
    std::vector<ComponentJson> components;
};
```

```cpp
BOWL_REFLECT(EntityDetail, index, generation, alive, components)
```

### ListEntities

Editor to game.

```cpp
struct ListEntities {
    u32 from = 0, count = 200;
    std::string filter; // part of the name or a component name; empty: all
};
```

```cpp
BOWL_REFLECT(ListEntities, from, count, filter)
```

### Watch

```cpp
struct Watch { // the game sends this entity's EntityDetail a few times a second; generation 0: none
    u32 index = 0, generation = 0;
};
```

```cpp
BOWL_REFLECT(Watch, index, generation)
```

### SetComponent

```cpp
struct SetComponent {
    u32 index = 0, generation = 0;
    std::string component, json;
};
```

```cpp
BOWL_REFLECT(SetComponent, index, generation, component, json)
```

### SetSystemEnabled

```cpp
struct SetSystemEnabled {
    std::string schedule; // "fixed" or "frame"
    std::string system;
    bool enabled = true;
};
```

```cpp
BOWL_REFLECT(SetSystemEnabled, schedule, system, enabled)
```

### TimeControl

```cpp
struct TimeControl {
    bool paused = false;
    bool step = false; // one fixed tick while paused
    f32 time_scale = 1;
};
```

```cpp
BOWL_REFLECT(TimeControl, paused, step, time_scale)
```

### GameEntry

A running game, from its file in the dev link folder.

```cpp
struct GameEntry {
    std::string game;
    u16 port = 0;
    std::string key;
    i64 pid = 0;
    std::filesystem::path file;
};
```

The folder games announce themselves in: &lt;temp>/bowl-devlink.

```cpp
std::filesystem::path folder();
```

The games running now. Files left by games that have exited are removed.

```cpp
std::vector<GameEntry> running_games();
```

### Sources

What the game's side reads and drives. Plain callbacks, so it runs without a window (tests).

```cpp
struct Sources
```

```cpp
std::string game;
ecs::World* world = nullptr;
AssetManager* assets = nullptr; // for components with Asset fields
ecs::Schedule* fixed = nullptr;
ecs::Schedule* frame = nullptr;
std::function<Frame()> stats;                       // the frame's numbers (zones are added by DevLink)
std::function<void(const TimeControl&)> time;       // pause, step, time scale
std::function<TimeControl()> time_state;
```

### DevLink

The game's side: listens, answers and streams. Call update() once a frame, outside the schedules.

```cpp
class DevLink : NonCopyable
```

```cpp
explicit DevLink(Sources sources);
~DevLink();
```

Starts listening and writes the announcement file (announce = false: no file, for tests). False if the socket
can't open.

```cpp
bool start(bool announce = true);
void update(f64 now = -1);
u16 port() const;
const std::string& key() const;
usize connections() const;
```

### DevLinkClient

The editor's side: connects to a running game and keeps what it last heard.

```cpp
class DevLinkClient : NonCopyable
```

```cpp
DevLinkClient();
~DevLinkClient();
```

```cpp
bool connect(const GameEntry& game);
bool connect(u16 port, const std::string& key); // to a game on this computer
void disconnect();
bool connected() const;
bool connecting() const;
void update(f64 now = -1);
```

What the game last said.

```cpp
const std::optional<Hello>& hello() const;
const std::optional<Schedules>& schedules() const;
const std::deque<Frame>& frames() const; // the last few seconds, oldest first
const std::optional<EntityPage>& page() const;
const std::optional<EntityDetail>& detail() const;
```

```cpp
void list_entities(const ListEntities& request);
void watch(u32 index, u32 generation);
void set_component(const SetComponent& change);
void set_system_enabled(const SetSystemEnabled& change);
void time(const TimeControl& control);
```

## bowl/app/engine.hpp

`#include "bowl/app/engine.hpp"` · namespace `bowl`

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
    std::string game_asset_dir;      // dev builds: the game's source asset folder (set by BOWL_MAIN)
    bool fullscreen_on_steam_deck = true; // see is_steam_deck() in bowl/platform/system.hpp
    // For tools built on the engine, like the editor: Game::dev_ui is always shown, not only while the F1 overlay
    // is open, and the engine's own windows move from F1 to F12. Dev builds only.
    bool dev_ui_always = false;
    // Dev builds: let Bowl Editor's Game tab attach to the running game (bowl/app/dev_link.hpp), on this computer only.
    bool dev_link = true;
};
```

### Game

Implement this, then put BOWL_MAIN(YourGame) in one .cpp file (see bowl/app/main.hpp).

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

## bowl/app/entry.hpp

`#include "bowl/app/entry.hpp"`

The engine side of the entry point. Games include bowl/app/main.hpp instead.

## bowl/app/hot_reload.hpp

`#include "bowl/app/hot_reload.hpp"` · namespace `bowl`

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
A plain-data component whose size or alignment changed loses its old data. Use BOWL_COMPONENT for savable components
so they're registered as soon as the library loads. State kept only in the Game object is rebuilt by init().

With source_dirs and build_command set (bowl_add_game does this), the host also rebuilds the library itself when a
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

## bowl/app/icon.hpp

`#include "bowl/app/icon.hpp"` · namespace `bowl`

The game's icon as a PNG, put into the executable by bowl_add_game(... ICON icon.png); empty without one. The App
shows it on the window (title bar, taskbar, and the Dock outside an .app bundle, which has its own .icns).

```cpp
std::span<const u8> embedded_icon();
```

### EmbeddedIcon

What the file bowl_add_game makes uses to register the icon.

```cpp
struct EmbeddedIcon
```

```cpp
EmbeddedIcon(const u8* png, usize size);
```

## bowl/app/inspector.hpp

`#include "bowl/app/inspector.hpp"` · namespace `bowl::inspector`

Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). Asset fields accept it.

```cpp
inline constexpr const char* asset_payload = "BOWL_ASSET_PATH";
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

## bowl/app/main.hpp

`#include "bowl/app/main.hpp"`

Include in exactly one .cpp of your game and write BOWL_MAIN(YourGame).

Uses SDL3's callback entry points instead of a main() loop. The web (Emscripten), iOS and Android need those, and
using them everywhere keeps the game code identical on every platform. In a hot-reload build
(bowl_add_game(... HOT_RELOAD)) the game is a library instead, and BOWL_MAIN exports the function the host uses to
create the game.

```cpp
#define BOWL_DETAIL_EXPORT extern "C" __declspec(dllexport)
#define BOWL_DETAIL_EXPORT extern "C" __attribute__((visibility("default")))
#define BOWL_MAIN(GameType)
```

```cpp
#define SDL_MAIN_USE_CALLBACKS 1
```

```cpp
#define BOWL_DETAIL_GAME_ASSETS BOWL_GAME_ASSET_SOURCE_DIR
#define BOWL_DETAIL_GAME_ASSETS ""
```

```cpp
#define BOWL_MAIN(GameType)
```

## bowl/app/scene.hpp

`#include "bowl/app/scene.hpp"` · namespace `bowl`

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

