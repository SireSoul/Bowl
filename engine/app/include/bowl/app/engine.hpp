#pragma once

#include "bowl/app/scene.hpp"
#include "bowl/audio/audio.hpp"
#include "bowl/core/assets.hpp"
#include "bowl/core/jobs.hpp"
#include "bowl/core/localization.hpp"
#include "bowl/core/log.hpp"
#include "bowl/core/settings.hpp"
#include "bowl/ecs/ecs.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/platform/input.hpp"
#include "bowl/platform/system.hpp"
#include "bowl/platform/window.hpp"

#include <memory>
#include <string>

union SDL_Event;

namespace bowl {

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

class Engine;

// Registers the engine's own components (transforms, names, sprites, maps, colliders...), so saved levels that hold
// them load by name. The engine does it at startup, before a hot-reloadable game library loads, so their code never
// belongs to a library that gets unloaded; tools and tests that load levels without an Engine call it themselves.
void register_engine_components();

// Implement this, then put BOWL_MAIN(YourGame) in one .cpp file (see bowl/app/main.hpp).
class Game {
public:
    virtual ~Game() = default;
    // Called first, before the window exists.
    virtual AppConfig configure() { return {}; }
    // Setup that code changes may need redone: register systems, bind inputs, load assets, set resources.
    // Runs at startup, and again after each game-code hot reload.
    virtual void init(Engine&) {}
    // Create the world (spawn entities). Runs once, after the first init; a hot reload keeps the world instead.
    virtual void start(Engine&) {}
    // Simulation at a fixed rate (after the fixed schedule). Deterministic code goes here.
    virtual void fixed_update(Engine&, f32 /*dt*/) {}
    // Once per frame (after the frame schedule): animation, camera, UI.
    virtual void update(Engine&, f32 /*dt*/) {}
    // Fill sprite batches and engine.renderer().submit() them.
    virtual void render(Engine&) {}
    // Dev builds: draw your own Dear ImGui windows here; shown with the overlay (F1).
    virtual void dev_ui(Engine&) {}
    virtual void event(Engine&, const SDL_Event&) {}
    virtual void shutdown(Engine&) {}
};

class DevTools;

// Every engine service, owned in one place and handed to the game.
class Engine : NonCopyable {
public:
    Engine();
    ~Engine();

    Window& window() { return window_; }
    gfx::GpuDevice& gpu() { return gpu_; }
    gfx::Renderer& renderer() { return renderer_; }
    Input& input() { return input_; }
    audio::AudioEngine& audio() { return audio_; }
    AssetManager& assets() { return assets_; }
    // Strings for the current language, from assets lang/<code>.json (see AppConfig::language).
    Localization& i18n() { return i18n_; }
    JobSystem& jobs() { return *jobs_; }
    ecs::World& world() { return world_; }
    // Runs every fixed tick, before Game::fixed_update.
    ecs::Schedule& fixed_schedule() { return fixed_schedule_; }
    // Runs every frame, before Game::update.
    ecs::Schedule& frame_schedule() { return frame_schedule_; }
    // The main world camera; its viewport follows the window.
    gfx::Camera2D& camera() { return camera_; }
    // Screens such as the title, gameplay and menus. They run after the Game's own callbacks each frame.
    SceneStack& scenes() { return scenes_; }
    // Player preferences, saved to user_data_path()/settings.json at exit. Controls saved under "input" (with
    // save_controls) are applied after Game::init, so player rebinds override the game's defaults.
    Settings& settings() { return settings_; }
    void save_controls() { settings_.set("input", input_.save_bindings()); }
    // A writable per-user folder for settings and save games (created on first use).
    const std::filesystem::path& user_data_path() const { return user_data_; }
    const AppConfig& config() const { return config_; }
    // Command-line arguments (argv[1..]), including the engine's own flags.
    const std::vector<std::string>& args() const { return args_; }
    bool has_arg(std::string_view flag) const {
        for (const auto& a : args_)
            if (a == flag) return true;
        return false;
    }

    f64 time() const { return time_; }               // simulated seconds (fixed ticks * fixed_dt)
    f32 delta() const { return delta_; }             // this frame, scaled
    f32 fixed_delta() const { return static_cast<f32>(1.0 / config_.fixed_hz); }
    // How far between the last two fixed ticks this frame is (0..1), for smooth rendering of fixed-rate motion.
    f32 alpha() const { return alpha_; }
    u64 frame() const { return frame_; }
    u64 tick() const { return tick_; }

    // Simulation controls (also on the dev overlay).
    void set_paused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    void step() { step_ = true; }
    void set_time_scale(f32 scale) { time_scale_ = scale; }
    f32 time_scale() const { return time_scale_; }

    void quit() { quit_ = true; }
    // In the background (phones and tablets): nothing updates or draws until the game comes back. Save the game when
    // SDL_EVENT_WILL_ENTER_BACKGROUND reaches Game::event, since the system may close it from there without warning.
    bool suspended() const { return suspended_; }

private:
    friend struct AppRunner;
    friend class DevTools;

    AppConfig config_;
    std::vector<std::string> args_;
    Settings settings_;
    std::filesystem::path user_data_;
    Window window_;
    gfx::GpuDevice gpu_;
    audio::AudioEngine audio_;
    std::unique_ptr<JobSystem> jobs_;
    Input input_;
    ecs::World world_;
    ecs::Schedule fixed_schedule_, frame_schedule_;
    gfx::Camera2D camera_;
    // Declared after the device: destroyed first, so textures and shaders release before the GPU goes away.
    AssetManager assets_;
    Localization i18n_{assets_};
    gfx::Renderer renderer_;
    SceneStack scenes_;

    f64 time_ = 0, accumulator_ = 0;
    f32 delta_ = 0, alpha_ = 0, time_scale_ = 1;
    u64 frame_ = 0, tick_ = 0;
    bool paused_ = false, step_ = false, quit_ = false, suspended_ = false;
};

} // namespace bowl
