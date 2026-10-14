#include "ph/core/assert.hpp"
#include "ph/core/log.hpp"
#include "ph/core/time.hpp"
#include "ph/core/type_id.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace ph {

// --- time -----------------------------------------------------------------------------------------------------------

namespace {
const auto process_start = std::chrono::steady_clock::now();
}

f64 now_seconds() { return std::chrono::duration<f64>(std::chrono::steady_clock::now() - process_start).count(); }

// --- type ids -------------------------------------------------------------------------------------------------------

TypeId detail::next_type_id() {
    static std::atomic<TypeId> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// --- logging --------------------------------------------------------------------------------------------------------

namespace {

struct Sink {
    LogSink fn;
    void* user;
};

struct LogState {
    std::mutex mutex;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::vector<Sink> sinks;
    std::deque<LogLine> recent;
};

LogState& log_state() {
    static LogState state;
    return state;
}

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info ";
    case LogLevel::Warn: return "warn ";
    case LogLevel::Error: return "error";
    }
    return "?";
}

} // namespace

void logging::set_level(LogLevel level) { log_state().level.store(level, std::memory_order_relaxed); }
LogLevel logging::level() { return log_state().level.load(std::memory_order_relaxed); }

void logging::add_sink(LogSink sink, void* user) {
    auto& s = log_state();
    std::lock_guard lock(s.mutex);
    s.sinks.push_back({sink, user});
}

void logging::remove_sink(LogSink sink, void* user) {
    auto& s = log_state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [&](const Sink& k) { return k.fn == sink && k.user == user; });
}

void logging::write(LogLevel level, std::string_view text) {
    auto& s = log_state();
    f64 t = now_seconds();
    std::lock_guard lock(s.mutex);
    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fprintf(out, "[%9.3f] %s | %.*s\n", t, level_name(level), static_cast<int>(text.size()), text.data());
    std::fflush(out);
    for (const Sink& sink : s.sinks) sink.fn(level, text, sink.user);
    s.recent.push_back({level, t, std::string(text)});
    if (s.recent.size() > 2048) s.recent.pop_front();
}

std::vector<LogLine> logging::recent(usize max_lines) {
    auto& s = log_state();
    std::lock_guard lock(s.mutex);
    usize n = std::min(max_lines, s.recent.size());
    return {s.recent.end() - static_cast<isize>(n), s.recent.end()};
}

// --- asserts --------------------------------------------------------------------------------------------------------

void detail::assert_fail(const char* condition, const char* file, int line, const char* message) {
    logging::write(LogLevel::Error, std::format("assertion failed: {}{}{}\n    at {}:{}", condition, message ? " — " : "",
                                                message ? message : "", file, line));
#if defined(_MSC_VER)
    __debugbreak();
#endif
    std::abort();
}

} // namespace ph
