#pragma once

#include "ph/core/types.hpp"

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ph {

enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error };

struct LogLine {
    LogLevel level;
    f64 time; // seconds since start
    std::string text;
};

using LogSink = void (*)(LogLevel level, std::string_view text, void* user);

namespace logging {
void set_level(LogLevel level);
LogLevel level();
// Sinks receive every line at or above the level, from any thread (calls are serialized).
void add_sink(LogSink sink, void* user);
void remove_sink(LogSink sink, void* user);
void write(LogLevel level, std::string_view text);
// The most recent lines, oldest first (for the dev console).
std::vector<LogLine> recent(usize max_lines = 512);
} // namespace logging

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (logging::level() <= LogLevel::Trace) logging::write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (logging::level() <= LogLevel::Debug) logging::write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (logging::level() <= LogLevel::Info) logging::write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (logging::level() <= LogLevel::Warn) logging::write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    logging::write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace ph
