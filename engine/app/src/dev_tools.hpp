#pragma once

#include "bowl/app/engine.hpp"

union SDL_Event;

namespace bowl {

// The in-game developer overlay (F1): stats, profiler, systems, entity browser, assets, log, time controls.
// Compiles to nothing when BOWL_DEV_TOOLS is off.
class DevTools : NonCopyable {
public:
    explicit DevTools(Engine& engine);
    ~DevTools();

    void handle_event(const SDL_Event& event);
    void begin_frame();
    void end_frame(Game& game);
    bool visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }
    void select(ecs::Entity e) {
#if BOWL_DEV_TOOLS
        selected_ = e;
#else
        (void)e;
#endif
    }

private:
#if BOWL_DEV_TOOLS
    void draw_main();
    void draw_profiler();
    void draw_post();
    void draw_systems(const char* label, ecs::Schedule& schedule);
    void draw_entities();
    void draw_inspector();
    void draw_assets();
    void draw_log();
    bool frame_open_ = false;
    ecs::Entity selected_;
    char filter_[128] = ""; // entity list filter
#endif
    [[maybe_unused]] Engine& engine_;
    bool visible_ = false;
};

} // namespace bowl
