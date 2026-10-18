// The other file for test_local_types.cpp: types with the same names as that file's, laid out differently.

#include "bowl/ecs/ecs.hpp"

#include <string>

using namespace bowl;
using namespace bowl::ecs;

namespace {

struct Timer {
    std::string label;
    f64 seconds = 0;
};
struct Settings {
    std::string name;
};

} // namespace

static ComponentId point_component() {
    struct Point {
        f64 x = 0, y = 0, z = 0;
    };
    return component_id<Point>();
}

namespace local_types_other {

std::string_view timer_name() { return type_name<Timer>(); }
ComponentId timer_component() { return component_id<Timer>(); }
TypeId timer_type_id() { return type_id<Timer>(); }
TypeId settings_type_id() { return type_id<Settings>(); }
ComponentId local_point_component() { return point_component(); }

void add_timer(World& world, Entity e, std::string label) { world.add<Timer>(e, Timer{std::move(label), 2.5}); }
std::string timer_label(World& world, Entity e) { return world.get<Timer>(e).label; }

void set_settings(World& world, std::string name) { world.set_resource<Settings>(Settings{std::move(name)}); }
std::string settings_name(World& world) { return world.resource<Settings>().name; }

} // namespace local_types_other
