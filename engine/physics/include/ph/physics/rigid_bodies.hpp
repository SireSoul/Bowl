#pragma once

// Rigid-body physics through Box2D v3: gravity, stacking, bouncing, things that tip over. Optional: build with
// -DPH_BOX2D=ON (PH_BOX2D is then 1 in code). Most games don't need it: move_and_slide and triggers cover precise
// character movement better.
//
// A body is an entity with a Transform2D, a Collider (its shape) and a RigidBody:
//
//     world.create(ecs::Transform2D{{0, 400}}, physics::Collider{.size = {800, 20}},
//                  physics::RigidBody{.type = physics::RigidBody::Type::Static});   // the floor
//     world.create(ecs::Transform2D{{0, 0}}, physics::Collider{.size = {24, 24}}, physics::RigidBody{});  // a crate
//
//     fixed.add("physics", [](ecs::SystemContext& ctx) { physics::step_physics(ctx.world, ctx.dt); });
//
// Bodies must be top-level entities (no Parent). Positions are in world units; PhysicsSettings::pixels_per_meter
// says how many make a meter, which is what Box2D's tuning (and the default gravity) is in.

#include "ph/physics/colliders.hpp"

namespace ph::physics {

struct RigidBody {
    enum class Type : u8 { Static, Kinematic, Dynamic };
    Type type = Type::Dynamic;
    f32 density = 1;        // per square meter; mass follows from the shape's area
    f32 friction = 0.6f;
    f32 restitution = 0;    // bounciness, 0..1
    f32 linear_damping = 0;
    f32 angular_damping = 0;
    f32 gravity_scale = 1;
    bool fixed_rotation = false; // characters usually want this
    bool bullet = false;         // extra care for fast, small things so they don't pass through others
    Vec2 velocity;               // world units per second; set it to push, read it to see
    f32 angular_velocity = 0;    // radians per second
};
PH_COMPONENT(RigidBody, type, density, friction, restitution, linear_damping, angular_damping, gravity_scale,
             fixed_rotation, bullet, velocity, angular_velocity)

// A world resource; added with defaults by the first step_physics.
struct PhysicsSettings {
    f32 pixels_per_meter = 32;
    Vec2 gravity{0, 9.81f * 32}; // world units per second squared; y points down
    int sub_steps = 4;
};

// Brings Box2D up to date with the world (new and removed bodies, positions and velocities the game changed, changed
// colliders), steps it by dt, and writes positions, rotations and velocities back.
void step_physics(ecs::World& world, f32 dt);

// Pushes a body: an instant change in momentum, in world units.
void apply_impulse(ecs::World& world, ecs::Entity e, Vec2 impulse);

} // namespace ph::physics
