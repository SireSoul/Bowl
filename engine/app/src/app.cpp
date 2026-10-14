#include "ph/app/entry.hpp"

#include "dev_tools.hpp"
#include "ph/core/profiler.hpp"
#include "ph/core/time.hpp"

#include <SDL3/SDL.h>

#include <charconv>
#include <filesystem>
#include <string_view>

namespace ph {

Engine::Engine() = default;
Engine::~Engine() = default;

struct AppRunner {
    std::unique_ptr<Game> game;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<DevTools> dev;
    u64 last_ticks = 0;
    f64 last_asset_poll = 0;
    u64 frame_limit = 0;        // --frames N: quit after N frames (automated runs)
    std::string screenshot;     // --screenshot file.png: capture the last frame (or frame 60 without --frames)
    bool start_with_overlay = false; // --overlay: open the dev overlay at startup

    bool init(int argc, char** argv, const char* game_asset_dir) {
        Engine& e = *engine;
        e.config_ = game->configure();
        for (int i = 1; i < argc; ++i) e.args_.emplace_back(argv[i]);
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--frames" && i + 1 < argc) {
                std::string_view n = argv[++i];
                std::from_chars(n.data(), n.data() + n.size(), frame_limit);
            } else if (arg == "--screenshot" && i + 1 < argc) {
                screenshot = argv[++i];
            } else if (arg == "--overlay") {
                start_with_overlay = true;
            } else if (arg == "--no-vsync") {
                e.config_.vsync = false;
            } else if (arg == "--driver" && i + 1 < argc) {
                e.config_.gpu_driver = argv[++i];
            }
        }
        if (e.config_.game_asset_dir.empty() && game_asset_dir) e.config_.game_asset_dir = game_asset_dir;
        logging::set_level(e.config_.log_level);
        info("PLACEHOLDERENGINENAME starting: {} on {}", e.config_.window.title, SDL_GetPlatform());

        SDL_SetAppMetadata(e.config_.window.title.c_str(), "0.1.0", nullptr);
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            error("SDL_Init failed: {}", SDL_GetError());
            return false;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) warn("audio: SDL audio unavailable: {}", SDL_GetError());

        e.jobs_ = std::make_unique<JobSystem>(e.config_.worker_threads);
        if (!e.window_.create(e.config_.window)) return false;
        gfx::GpuDeviceDesc gpu_desc;
        gpu_desc.vsync = e.config_.vsync;
        gpu_desc.driver = e.config_.gpu_driver;
        if (!e.gpu_.init(e.window_.handle(), gpu_desc)) return false;
        e.audio_.init();

        mount_assets(e);
        gfx::register_asset_loaders(e.assets_, e.gpu_, e.config_.pixel_art ? gfx::Filter::Nearest : gfx::Filter::Linear);
        audio::register_asset_loaders(e.assets_);
        e.assets_.set_hot_reload(PH_DEV_TOOLS != 0);
        if (!e.renderer_.init(e.gpu_, e.assets_)) return false;

        e.camera_.viewport = e.window_.size();
        e.camera_.position = e.camera_.viewport * 0.5f;
        e.camera_.pixel_snap = e.config_.pixel_art;
        e.world_.set_resource<Engine*>(&e);

        dev = std::make_unique<DevTools>(e);
        dev->set_visible(start_with_overlay);
        game->init(e);
        last_ticks = SDL_GetTicksNS();
        info("ready: {} worker threads, {} backend", e.jobs_->worker_count(), e.gpu_.driver());
        return true;
    }

    void mount_assets(Engine& e) {
        namespace fs = std::filesystem;
        const char* base_path = SDL_GetBasePath();
        fs::path base = base_path ? fs::path(base_path) : fs::current_path();
#if PH_DEV_TOOLS && defined(PH_ENGINE_ASSET_SOURCE_DIR)
        // Dev builds read straight from the source tree, so saved files hot-reload.
        e.assets_.mount("engine/", PH_ENGINE_ASSET_SOURCE_DIR);
        if (!e.config_.game_asset_dir.empty()) e.assets_.mount("", e.config_.game_asset_dir);
#endif
        e.assets_.mount("engine/", base / "engine");
        e.assets_.mount("", base / "assets");
    }

    SDL_AppResult iterate() {
        Engine& e = *engine;
        profiler::begin_frame();
        u64 now = SDL_GetTicksNS();
        f64 real_dt = std::min(static_cast<f64>(now - last_ticks) / 1e9, 0.25);
        last_ticks = now;

        e.input_.end_events();
        if (e.assets_.hot_reload() && now_seconds() - last_asset_poll > 0.25) {
            PH_PROFILE_SCOPE("assets.poll");
            last_asset_poll = now_seconds();
            e.assets_.poll();
        }
        e.camera_.viewport = e.window_.size();
        dev->begin_frame();

        // --- fixed-rate simulation ---------------------------------------------------------------------------------
        f64 step = 1.0 / e.config_.fixed_hz;
        f64 scaled = real_dt * e.time_scale_;
        if (!e.paused_) e.accumulator_ += scaled;
        else if (e.step_) e.accumulator_ = step;
        i32 steps = 0;
        e.input_.set_context(Input::Context::Fixed);
        while (e.accumulator_ >= step && steps < e.config_.max_fixed_steps) {
            PH_PROFILE_SCOPE("fixed tick");
            e.input_.begin_fixed_tick();
            e.fixed_schedule_.run(e.world_, *e.jobs_, static_cast<f32>(step));
            game->fixed_update(e, static_cast<f32>(step));
            e.world_.flush();
            e.input_.end_fixed_tick();
            e.accumulator_ -= step;
            e.time_ += step;
            ++e.tick_;
            ++steps;
        }
        if (steps == e.config_.max_fixed_steps) e.accumulator_ = 0; // too slow: drop time rather than spiral
        e.step_ = false;
        e.alpha_ = static_cast<f32>(e.accumulator_ / step);

        // --- per-frame update --------------------------------------------------------------------------------------
        e.input_.set_context(Input::Context::Frame);
        e.delta_ = e.paused_ ? 0.0f : static_cast<f32>(scaled);
        {
            PH_PROFILE_SCOPE("frame update");
            e.frame_schedule_.run(e.world_, *e.jobs_, e.delta_);
            game->update(e, e.delta_);
            e.world_.flush();
        }

        // --- render ------------------------------------------------------------------------------------------------
        if (!screenshot.empty() && e.frame_ + 1 == (frame_limit ? frame_limit : 60)) {
            e.renderer_.capture_next_frame([path = screenshot](u32 w, u32 h, std::span<const u8> rgba) {
                if (gfx::write_png(path, w, h, rgba)) info("screenshot: {} ({}x{})", path, w, h);
                else error("screenshot: could not write {}", path);
            });
        }
        if (e.renderer_.begin_frame()) {
            PH_PROFILE_SCOPE("game render");
            game->render(e);
        }
        dev->end_frame(*game);
        e.renderer_.end_frame();

        e.input_.begin_frame();
        profiler::end_frame();
        ++e.frame_;
        if (e.quit_ || (frame_limit && e.frame_ >= frame_limit)) return SDL_APP_SUCCESS;
        return SDL_APP_CONTINUE;
    }

    SDL_AppResult event(SDL_Event& ev) {
        Engine& e = *engine;
        if (ev.type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
        dev->handle_event(ev);
        e.input_.handle_event(ev);
        game->event(e, ev);
        return SDL_APP_CONTINUE;
    }

    void shutdown() {
        if (engine && game) game->shutdown(*engine);
        if (engine) SDL_WaitForGPUIdle(engine->gpu_.handle());
        dev.reset();
        game.reset();
        engine.reset();
        SDL_Quit();
    }
};

namespace detail {

SDL_AppResult app_init(void** state, int argc, char** argv, GameFactory factory, const char* game_asset_dir) {
    auto* runner = new AppRunner();
    *state = runner;
    runner->game = factory();
    runner->engine = std::make_unique<Engine>();
    if (!runner->init(argc, argv, game_asset_dir)) {
        error("startup failed");
        return SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult app_iterate(void* state) { return static_cast<AppRunner*>(state)->iterate(); }

SDL_AppResult app_event(void* state, SDL_Event* event) { return static_cast<AppRunner*>(state)->event(*event); }

void app_quit(void* state, SDL_AppResult result) {
    auto* runner = static_cast<AppRunner*>(state);
    if (!runner) return;
    runner->shutdown();
    delete runner;
    info("exit: {}", result == SDL_APP_FAILURE ? "failure" : "ok");
}

} // namespace detail
} // namespace ph
