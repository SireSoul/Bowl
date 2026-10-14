#pragma once

#include "ph/core/types.hpp"

#include <string_view>
#include <vector>

namespace ph {

// A CPU profiler that's always compiled in and cheap enough to leave on. Zones with the same name add up per frame,
// and the dev overlay shows the totals. Names are copied the first time they're seen, so any string works.
//
//     void physics() { PH_PROFILE_SCOPE("physics"); ... }
namespace profiler {

struct ZoneStats {
    std::string_view name;
    f64 milliseconds = 0; // total across all calls and threads this frame
    u32 calls = 0;
};

void set_enabled(bool enabled);
bool enabled();
void begin_frame();
void end_frame();
void record(std::string_view name, f64 milliseconds);

// Stats of the last completed frame, sorted by time descending.
std::vector<ZoneStats> last_frame();
// Total frame time history (ms), oldest first.
std::vector<f32> frame_history();

} // namespace profiler

class ProfileScope {
public:
    explicit ProfileScope(std::string_view name);
    ~ProfileScope();
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    std::string_view name_;
    f64 start_;
};

#define PH_PROFILE_SCOPE(name) ::ph::ProfileScope PH_CONCAT(ph_profile_scope_, __LINE__){name}

} // namespace ph
