#include "ph/core/profiler.hpp"

#include "ph/core/time.hpp"

#if PH_TRACY
#include <tracy/TracyC.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ph {

namespace {

struct ProfilerState {
    std::atomic<bool> enabled{true};
    std::mutex mutex;
    // Zone names are copied once and reused: a name may point into a game library that gets hot-reloaded.
    std::unordered_map<std::string_view, std::unique_ptr<std::string>> names; // key views its own value
    std::unordered_map<std::string_view, profiler::ZoneStats> current;
    std::vector<profiler::ZoneStats> last;
    std::deque<f32> history;
    f64 frame_start = 0;
};

ProfilerState& state() {
    static ProfilerState s;
    return s;
}

} // namespace

void profiler::set_enabled(bool enabled) { state().enabled.store(enabled, std::memory_order_relaxed); }
bool profiler::enabled() { return state().enabled.load(std::memory_order_relaxed); }

void profiler::begin_frame() { state().frame_start = now_seconds(); }

void profiler::end_frame() {
    auto& s = state();
    f64 frame_ms = (now_seconds() - s.frame_start) * 1000.0;
    std::lock_guard lock(s.mutex);
    s.last.clear();
    for (auto& [name, stats] : s.current) s.last.push_back(stats);
    std::sort(s.last.begin(), s.last.end(), [](const ZoneStats& a, const ZoneStats& b) { return a.milliseconds > b.milliseconds; });
    s.current.clear();
    s.history.push_back(static_cast<f32>(frame_ms));
    if (s.history.size() > 240) s.history.pop_front();
#if PH_TRACY
    ___tracy_emit_frame_mark(nullptr);
#endif
}

void profiler::set_thread_name([[maybe_unused]] const char* name) {
#if PH_TRACY
    ___tracy_set_thread_name(name);
#endif
}

void profiler::plot([[maybe_unused]] const char* name, [[maybe_unused]] f64 value) {
#if PH_TRACY
    ___tracy_emit_plot(name, value);
#endif
}

bool profiler::tracy_enabled() {
#if PH_TRACY
    return true;
#else
    return false;
#endif
}

void profiler::record(std::string_view name, f64 milliseconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto interned = s.names.find(name);
    if (interned == s.names.end()) {
        auto owned = std::make_unique<std::string>(name);
        std::string_view view = *owned;
        interned = s.names.emplace(view, std::move(owned)).first;
    }
    std::string_view stable = interned->first;
    auto& z = s.current[stable];
    z.name = stable;
    z.milliseconds += milliseconds;
    ++z.calls;
}

std::vector<profiler::ZoneStats> profiler::last_frame() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.last;
}

std::vector<f32> profiler::frame_history() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return {s.history.begin(), s.history.end()};
}

ProfileScope::ProfileScope(std::string_view name, [[maybe_unused]] const char* file, [[maybe_unused]] u32 line)
    : name_(name), start_(profiler::enabled() ? now_seconds() : -1.0) {
#if PH_TRACY
    // Names are often runtime strings (system names), so the source location is allocated per zone. Tracy copies it.
    if (___tracy_connected()) {
        const char* f = file ? file : "";
        u64 srcloc = ___tracy_alloc_srcloc_name(line, f, std::strlen(f), "", 0, name.data(), name.size(), 0);
        TracyCZoneCtx ctx = ___tracy_emit_zone_begin_alloc(srcloc, 1);
        tracy_zone_ = ctx.id;
        tracy_active_ = ctx.active != 0;
    }
#endif
}

ProfileScope::~ProfileScope() {
    if (start_ >= 0) profiler::record(name_, (now_seconds() - start_) * 1000.0);
#if PH_TRACY
    if (tracy_active_) ___tracy_emit_zone_end(TracyCZoneCtx{tracy_zone_, 1});
#endif
}

} // namespace ph
