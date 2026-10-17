#pragma once

#include "bowl/app/engine.hpp"

#include <string>
#include <vector>

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
    // The profiler table: totals gathered for half a second, then shown as averages per frame in a fixed order, so rows
    // don't jump about as zones trade places or skip frames (a fixed tick doesn't run every frame).
    struct ZoneRow {
        std::string name;
        f64 ms = 0, calls = 0; // per frame: averages shown (rows), or totals so far (gathering)
    };
    std::vector<ZoneRow> zone_rows_, zone_totals_;
    u32 zone_frames_ = 0;
    u64 zone_window_start_ns_ = 0;
#endif
    [[maybe_unused]] Engine& engine_;
    bool visible_ = false;
};

} // namespace bowl
