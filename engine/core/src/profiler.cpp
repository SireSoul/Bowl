#include "ph/core/profiler.hpp"

#include "ph/core/time.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace ph {

namespace {

struct ProfilerState {
    std::atomic<bool> enabled{true};
    std::mutex mutex;
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
}

void profiler::record(std::string_view name, f64 milliseconds) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto& z = s.current[name];
    z.name = name;
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

ProfileScope::ProfileScope(std::string_view name) : name_(name), start_(profiler::enabled() ? now_seconds() : -1.0) {}

ProfileScope::~ProfileScope() {
    if (start_ >= 0) profiler::record(name_, (now_seconds() - start_) * 1000.0);
}

} // namespace ph
