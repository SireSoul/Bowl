// Only built with PH_BOX2D.
#include "ph/ecs/world_io.hpp"
#include "ph/physics/rigid_bodies.hpp"

#include <doctest/doctest.h>

using namespace ph;
using namespace ph::physics;

TEST_CASE("rigid bodies: a crate falls and lands on the floor") {
    ecs::World world;
    world.create(ecs::Transform2D{{0, 100}}, Collider{.size = {400, 20}}, RigidBody{.type = RigidBody::Type::Static});
    ecs::Entity crate = world.create(ecs::Transform2D{{0, 0}}, Collider{.size = {20, 20}}, RigidBody{});
    for (int i = 0; i < 180; ++i) step_physics(world, 1.0f / 60.0f); // three seconds
    const ecs::Transform2D& t = world.get<ecs::Transform2D>(crate);
    CHECK(t.position.y == doctest::Approx(80).epsilon(0.02)); // resting on top: floor top 90, half a crate up
    CHECK(std::abs(world.get<RigidBody>(crate).velocity.y) < 1.0f);
}

TEST_CASE("rigid bodies: the game can move and push bodies") {
    ecs::World world;
    world.set_resource<PhysicsSettings>(PhysicsSettings{.gravity = {0, 0}});
    ecs::Entity ball = world.create(ecs::Transform2D{{0, 0}}, Collider{.kind = Collider::Kind::Circle, .radius = 5}, RigidBody{});
    step_physics(world, 1.0f / 60.0f);
    world.get<ecs::Transform2D>(ball).position = {500, 0}; // teleport
    world.get<RigidBody>(ball).velocity = {60, 0};          // and set off
    step_physics(world, 1.0f);
    CHECK(world.get<ecs::Transform2D>(ball).position.x == doctest::Approx(560).epsilon(0.01));
    apply_impulse(world, ball, {0, 100});
    step_physics(world, 0.0f);
    CHECK(world.get<RigidBody>(ball).velocity.y > 0);
    world.remove<RigidBody>(ball); // the body goes with the component
    step_physics(world, 1.0f / 60.0f);
}

TEST_CASE("rigid bodies: a pendulum on a hinge keeps its length and swings") {
    ecs::World world;
    ecs::Entity pivot = world.create(ecs::Transform2D{{0, 0}}, Collider{.size = {4, 4}}, RigidBody{.type = RigidBody::Type::Static});
    ecs::Entity bob = world.create(ecs::Transform2D{{64, 0}}, Collider{.kind = Collider::Kind::Circle, .radius = 6}, RigidBody{});
    world.create(Joint{.kind = Joint::Kind::Revolute, .a = pivot, .b = bob, .anchor_a = {0, 0}, .anchor_b = {-64, 0}});
    f32 lowest = 0;
    for (int i = 0; i < 120; ++i) {
        step_physics(world, 1.0f / 60.0f);
        Vec2 p = world.get<ecs::Transform2D>(bob).position;
        CHECK(p.length() == doctest::Approx(64).epsilon(0.03)); // held at the end of the arm
        lowest = std::max(lowest, p.y);
    }
    CHECK(lowest == doctest::Approx(64).epsilon(0.05)); // swung down to straight below the pivot
}

TEST_CASE("rigid bodies: a rope only pulls, and joints go with their bodies") {
    ecs::World world;
    ecs::Entity hook = world.create(ecs::Transform2D{{0, 0}}, Collider{.size = {4, 4}}, RigidBody{.type = RigidBody::Type::Static});
    ecs::Entity weight = world.create(ecs::Transform2D{{0, 10}}, Collider{.size = {8, 8}}, RigidBody{});
    ecs::Entity rope = world.create(Joint{.kind = Joint::Kind::Distance, .a = hook, .b = weight, .length = 10, .max_length = 50});
    for (int i = 0; i < 240; ++i) step_physics(world, 1.0f / 60.0f);
    CHECK(world.get<ecs::Transform2D>(weight).position.y == doctest::Approx(50).epsilon(0.05)); // fell to the rope's end
    world.destroy(hook);
    for (int i = 0; i < 60; ++i) step_physics(world, 1.0f / 60.0f);
    CHECK(world.get<ecs::Transform2D>(weight).position.y > 60); // free: it falls
    CHECK(world.alive(rope)); // the joint's entity stays; the joint itself is gone
}

TEST_CASE("rigid bodies: contact events report touches and hard hits") {
    ecs::World world;
    ecs::Entity floor = world.create(ecs::Transform2D{{0, 100}}, Collider{.size = {400, 20}}, RigidBody{.type = RigidBody::Type::Static});
    ecs::Entity crate = world.create(ecs::Transform2D{{0, 0}}, Collider{.size = {20, 20}}, RigidBody{});
    bool began = false, hit = false;
    f32 fastest = 0;
    for (int i = 0; i < 120; ++i) {
        step_physics(world, 1.0f / 60.0f);
        const auto& events = world.resource<ContactEvents>();
        for (const auto& t : events.began)
            began = began || ((t.a == floor && t.b == crate) || (t.a == crate && t.b == floor));
        for (const auto& h : events.hits) {
            hit = true;
            fastest = std::max(fastest, h.speed);
        }
    }
    CHECK(began);
    CHECK(hit);
    CHECK(fastest > 100); // it fell about 70 units: well over 100 units per second at impact
}

TEST_CASE("rigid bodies: joints save and load with the world, references and all") {
    ecs::register_components<RigidBody, Joint>();
    ecs::World a;
    ecs::Entity pivot = a.create(ecs::Transform2D{{0, 0}}, Collider{.size = {4, 4}}, RigidBody{.type = RigidBody::Type::Static});
    ecs::Entity bob = a.create(ecs::Transform2D{{0, 40}}, Collider{.size = {4, 4}}, RigidBody{});
    a.create(Joint{.kind = Joint::Kind::Weld, .a = pivot, .b = bob, .anchor_b = {0, -40}});
    serial::Context ctx;
    ecs::World b;
    b.create(); // offset ids
    auto loaded = ecs::load_entities(b, ecs::save_world(a, ctx), ctx);
    REQUIRE(loaded.size() == 3);
    const Joint& j = b.get<Joint>(loaded[2]);
    CHECK(j.a == loaded[0]);
    CHECK(j.b == loaded[1]);
    for (int i = 0; i < 60; ++i) step_physics(b, 1.0f / 60.0f);
    CHECK(b.get<ecs::Transform2D>(loaded[1]).position.y == doctest::Approx(40).epsilon(0.02)); // welded: no fall
}
