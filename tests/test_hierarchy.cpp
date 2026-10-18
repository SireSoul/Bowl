#include "bowl/ecs/ecs.hpp"
#include "bowl/ecs/hierarchy.hpp"

#include <doctest/doctest.h>

using namespace bowl;
using namespace bowl::ecs;

TEST_CASE("hierarchy: children follow their parent's position, rotation and scale") {
    World w;
    Entity ship = w.create(Transform2D{{100, 50}, 0, {2, 2}});
    Entity turret = w.create(Transform2D{{10, 0}});
    Entity barrel = w.create(Transform2D{{5, 0}});
    REQUIRE(set_parent(w, turret, ship));
    REQUIRE(set_parent(w, barrel, turret));
    propagate_transforms(w);
    CHECK(w.get<GlobalTransform2D>(turret).position == Vec2{120, 50}); // 10 * scale 2
    CHECK(w.get<GlobalTransform2D>(barrel).position == Vec2{130, 50});

    w.get<Transform2D>(ship).rotation = pi / 2; // quarter turn: +x becomes +y
    propagate_transforms(w);
    Vec2 b = w.get<GlobalTransform2D>(barrel).position;
    CHECK(b.x == doctest::Approx(100).epsilon(1e-4));
    CHECK(b.y == doctest::Approx(80).epsilon(1e-4));
    CHECK(w.get<GlobalTransform2D>(barrel).rotation == doctest::Approx(pi / 2));
}

TEST_CASE("hierarchy: reparenting, detaching and cycle refusal") {
    World w;
    Entity a = w.create(Transform2D{{1, 0}});
    Entity b = w.create(Transform2D{{2, 0}});
    Entity c = w.create(Transform2D{{3, 0}});
    REQUIRE(set_parent(w, b, a));
    REQUIRE(set_parent(w, c, b));
    CHECK_FALSE(set_parent(w, a, c)); // a -> b -> c -> a would loop
    CHECK_FALSE(set_parent(w, a, a));

    REQUIRE(set_parent(w, c, a)); // move c from b to a
    CHECK(w.get<Children>(b).list.empty());
    CHECK(w.get<Children>(a).list.size() == 2);
    REQUIRE(set_parent(w, c, null_entity)); // detach
    CHECK_FALSE(w.has<Parent>(c));
    propagate_transforms(w);
    CHECK(w.get<GlobalTransform2D>(c).position == Vec2{3, 0}); // now a root
}

TEST_CASE("hierarchy: destroy_recursive removes the whole subtree") {
    World w;
    Entity root = w.create(Transform2D{});
    Entity keep = w.create(Transform2D{});
    Entity mid = w.create(Transform2D{});
    Entity leaf = w.create(Transform2D{});
    set_parent(w, mid, root);
    set_parent(w, leaf, mid);
    set_parent(w, keep, root);
    destroy_recursive(w, mid);
    CHECK_FALSE(w.alive(mid));
    CHECK_FALSE(w.alive(leaf));
    CHECK(w.alive(root));
    CHECK(w.get<Children>(root).list == std::vector<Entity>{keep});
}

TEST_CASE("hierarchy: saves and loads with references intact") {
    World w;
    Entity parent = w.create(Transform2D{{10, 10}});
    Entity child = w.create(Transform2D{{1, 1}});
    set_parent(w, child, parent);
    serial::Context ctx;
    json::Value saved = save_world(w, ctx);

    World other;
    other.create(); // shift ids
    auto loaded = load_entities(other, saved, ctx);
    REQUIRE(loaded.size() == 2);
    Entity p2 = loaded[0], c2 = loaded[1];
    CHECK(other.get<Parent>(c2).entity == p2);
    CHECK(other.get<Children>(p2).list == std::vector<Entity>{c2});
    propagate_transforms(other);
    CHECK(other.get<GlobalTransform2D>(c2).position == Vec2{11, 11});
}

TEST_CASE("hierarchy: a child whose parent was destroyed on its own becomes a root") {
    World w;
    Entity parent = w.create(Transform2D{{100, 0}});
    Entity child = w.create(Transform2D{{5, 0}});
    REQUIRE(set_parent(w, child, parent));
    propagate_transforms(w);
    CHECK(w.get<GlobalTransform2D>(child).position == Vec2{105, 0});
    w.destroy(parent); // not destroy_recursive: the child keeps a Parent that's gone
    w.get<Transform2D>(child).position.x = 7;
    propagate_transforms(w);
    CHECK(w.get<GlobalTransform2D>(child).position == Vec2{7, 0}); // still followed, from the origin
}

TEST_CASE("hierarchy: a cycle in loaded data doesn't hang anything") {
    World w;
    Entity a = w.create(Transform2D{}), b = w.create(Transform2D{}), c = w.create(Transform2D{});
    // As a hand-edited save might have it: a and b are each other's parent.
    w.add<Parent>(a, b);
    w.add<Parent>(b, a);
    w.add<Children>(a, std::vector<Entity>{b});
    w.add<Children>(b, std::vector<Entity>{a});
    propagate_transforms(w);
    CHECK_FALSE(set_parent(w, c, a)); // refused: walking up from a never ends
    destroy_recursive(w, a);
    CHECK_FALSE(w.alive(a));
    CHECK_FALSE(w.alive(b));
    CHECK(w.alive(c));
}

TEST_CASE("world io: names are the same on every compiler, and GCC's old shorter ones still load") {
    CHECK(type_name<Transform2D>() == "bowl::ecs::Transform2D");
    // A GCC build used to write engine types without "bowl::".
    auto doc = json::parse(R"({ "format": "Bowl.world", "version": 1, "entities": [
        { "id": 0, "components": { "ecs::Transform2D": { "position": [3, 4] }, "ecs::Name": { "value": "old" } } } ] })");
    REQUIRE(doc);
    register_components<Transform2D, Name>();
    World w;
    serial::Context ctx;
    auto made = load_entities(w, doc.value, ctx);
    REQUIRE(made.size() == 1);
    CHECK(w.get<Name>(made[0]).value == "old");
    CHECK(w.get<Transform2D>(made[0]).position == Vec2{3, 4});
    CHECK(ctx.warnings.empty());
}
