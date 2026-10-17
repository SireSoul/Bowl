// Types whose names don't identify them: every file's anonymous namespace prints the same, and so can types declared
// in functions. local_types_other.cpp declares types with the same names as these, laid out differently.

#include "bowl/ecs/ecs.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace bowl;
using namespace bowl::ecs;

namespace {

struct Timer {
    f32 seconds = 0;
};
struct Settings {
    int volume = 0;
};

} // namespace

static ComponentId point_component() {
    struct Point {
        i8 x = 0;
    };
    return component_id<Point>();
}

namespace local_types_other {
std::string_view timer_name();
ComponentId timer_component();
TypeId timer_type_id();
TypeId settings_type_id();
ComponentId local_point_component();
void add_timer(World& world, Entity e, std::string label);
std::string timer_label(World& world, Entity e);
void set_settings(World& world, std::string name);
std::string settings_name(World& world);
} // namespace local_types_other

namespace local_types_test {
struct Named {
    int value = 0;
};
} // namespace local_types_test

namespace other = local_types_other;

TEST_CASE("local types: same-named types in two files' anonymous namespaces get their own component ids") {
    REQUIRE(type_name<Timer>() == other::timer_name()); // what used to make them one type
    CHECK(has_local_name<Timer>());

    ComponentId mine = component_id<Timer>();
    ComponentId theirs = other::timer_component();
    CHECK(mine != theirs);
    CHECK(component_id<const Timer>() == mine);
    CHECK(component_info(mine).size == sizeof(Timer));
    CHECK(component_info(theirs).size != sizeof(Timer));

    // Both are listed by name: the plain one for whichever came first, a numbered one for the other.
    std::string_view a = component_info(mine).name, b = component_info(theirs).name;
    CHECK(a != b);
    CHECK((a == type_name<Timer>() || b == type_name<Timer>()));
    CHECK(find_component(a) == mine);
    CHECK(find_component(b) == theirs);

    // One entity holds both, and neither overwrites the other.
    World world;
    Entity e = world.create();
    world.add<Timer>(e, Timer{1.5f});
    other::add_timer(world, e, "a label long enough to live on the heap, not in the string itself");
    world.add<Timer>(e, Timer{3.0f}); // replaces only this file's Timer
    CHECK(world.get<Timer>(e).seconds == 3.0f);
    CHECK(other::timer_label(world, e) == "a label long enough to live on the heap, not in the string itself");
    world.remove<Timer>(e);
    CHECK_FALSE(world.has<Timer>(e));
    CHECK(other::timer_label(world, e) == "a label long enough to live on the heap, not in the string itself");
}

TEST_CASE("local types: same-named types in two files' anonymous namespaces get their own type ids") {
    CHECK(type_id<Timer>() != other::timer_type_id());
    CHECK(type_id<Settings>() != other::settings_type_id());
    CHECK(type_id<Timer>() == type_id<const Timer&>());

    World world;
    world.set_resource<Settings>(Settings{7});
    other::set_settings(world, "a name long enough to live on the heap, not in the string itself");
    CHECK(world.resource<Settings>().volume == 7);
    CHECK(other::settings_name(world) == "a name long enough to live on the heap, not in the string itself");
}

TEST_CASE("local types: same-named types declared in two files' functions get their own component ids") {
    ComponentId mine = point_component(), theirs = other::local_point_component();
    CHECK(mine != theirs);
    CHECK(component_info(mine).size == 1);
    CHECK(component_info(theirs).size == 3 * sizeof(f64));
}

TEST_CASE("local types: named types keep one id in every module") {
    using local_types_test::Named;
    CHECK_FALSE(has_local_name<Named>());
    CHECK(type_id<Named>() == fnv1a("local_types_test::Named")); // the same wherever it's computed

    // Another module registering the same named type (its own anchors, same shape) shares the id, as a hot-reloaded
    // game library and its host must.
    ComponentId id = component_id<Named>();
    static char other_module, other_anchor, another_anchor;
    ComponentInfo info = ecs::detail::make_component_info<Named>();
    info.module_key = &other_module;
    info.type_key = &other_anchor;
    CHECK(ecs::detail::register_component(info) == id);

    // But not if the shape differs (it can't be the same type), nor if it's another anchor in the same module.
    ComponentInfo wider = info;
    wider.size = 2 * sizeof(Named);
    CHECK(ecs::detail::register_component(wider) != id);
    ComponentInfo same_module = ecs::detail::make_component_info<Named>();
    same_module.type_key = &another_anchor;
    CHECK(ecs::detail::register_component(same_module) != id);
}

TEST_CASE("local types: a hot reload's new code takes over its dormant registration") {
    ComponentId id = component_id<Timer>();
    auto saved = mark_components_dormant(
        [](const ComponentInfo& info, void*) { return info.type_key == &bowl::detail::type_anchor<Timer>; }, nullptr);
    REQUIRE(saved.size() == 1);
    CHECK(component_info(id).dormant);

    // The new library's Timer has an anchor of its own; it still gets the id (and the data kept under it).
    static char new_anchor;
    ComponentInfo reloaded = ecs::detail::make_component_info<Timer>();
    reloaded.type_key = &new_anchor;
    CHECK(ecs::detail::register_component(reloaded) == id);
    CHECK_FALSE(component_info(id).dormant);
    CHECK(component_info(id).name == saved[0].second.name);

    restore_components(saved);
    CHECK(component_info(id).type_key == &bowl::detail::type_anchor<Timer>);
}
