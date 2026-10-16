#pragma once

#include "bowl/core/types.hpp"

#include <chrono>

namespace bowl {

// Seconds since the process started, from a monotonic clock.
f64 now_seconds();

class Stopwatch {
public:
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}
    void reset() { start_ = std::chrono::steady_clock::now(); }
    f64 seconds() const { return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start_).count(); }
    f64 milliseconds() const { return seconds() * 1000.0; }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace bowl
