#pragma once

#include "ph/app/scene.hpp"
#include "ph/audio/audio.hpp"
#include "ph/core/assets.hpp"
#include "ph/core/jobs.hpp"
#include "ph/core/localization.hpp"
#include "ph/core/log.hpp"
#include "ph/ecs/ecs.hpp"
#include "ph/gfx/renderer.hpp"
#include "ph/platform/input.hpp"
#include "ph/platform/window.hpp"

#include <memory>
#include <string>

union SDL_Event;

namespace ph {

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
    std::string game_asset_dir;      // dev builds: the game's source asset folder (set by PH_MAIN)
};

class Engine;

// Implement this, then put PH_MAIN(YourGame) in one .cpp file (see ph/app/main.hpp).
class Game {
public:
    virtual ~Game() = default;
    // Called first, before the window exists.
    virtual AppConfig configure() { return {}; }
    virtual void init(Engine&) {}
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
    // Title screen, gameplay, menus...: run after the Game's own callbacks each frame.
    SceneStack& scenes() { return scenes_; }
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

private:
    friend struct AppRunner;
    friend class DevTools;

    AppConfig config_;
    std::vector<std::string> args_;
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
    bool paused_ = false, step_ = false, quit_ = false;
};

} // namespace ph
