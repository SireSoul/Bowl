// Only built with PH_BOX2D.
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
