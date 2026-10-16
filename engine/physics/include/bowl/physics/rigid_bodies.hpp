#pragma once

// Rigid-body physics through Box2D v3: gravity, stacking, bouncing, things that tip over. Optional: build with
// -DBOWL_BOX2D=ON (BOWL_BOX2D is then 1 in code). Most games don't need it: move_and_slide and triggers cover precise
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

#include "bowl/physics/colliders.hpp"

namespace bowl::physics {

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
BOWL_COMPONENT(RigidBody, type, density, friction, restitution, linear_damping, angular_damping, gravity_scale,
             fixed_rotation, bullet, velocity, angular_velocity)

// A world resource; added with defaults by the first step_physics.
struct PhysicsSettings {
    f32 pixels_per_meter = 32;
    Vec2 gravity{0, 9.81f * 32}; // world units per second squared; y points down
    int sub_steps = 4;
    f32 hit_speed = 32;          // ContactEvents::hits only reports impacts faster than this (world units per second)
};

// A joint between two bodies, on an entity of its own (it's saved and loaded with the world like anything else, and
// the two body references survive). Anchors are in each body's own space, in world units. What each kind does:
//   Distance   keeps the anchors `length` apart: a rod, a bungee with a spring, a rope with max_length
//   Revolute   a hinge at the anchors: doors, wheels, pendulums; limits in radians, a motor turns it
//   Prismatic  a slider along `axis` (in body a's space): pistons, elevators; limits and motor along the axis
//   Weld       glues two bodies; a spring lets it bend (wobbly bridges)
//   Wheel      a wheel on a suspension along `axis`: cars; the spring is the suspension, the motor drives it
// Changing a field rebuilds the joint. Removing the component, or destroying either body, removes it.
struct Joint {
    enum class Kind : u8 { Distance, Revolute, Prismatic, Weld, Wheel };
    Kind kind = Kind::Revolute;
    ecs::Entity a, b;
    Vec2 anchor_a, anchor_b;
    Vec2 axis{0, 1};                    // Prismatic, Wheel
    f32 length = 0;                     // Distance: 0 keeps the distance the anchors have when the joint is made
    f32 min_length = 0, max_length = 0; // Distance: a rope when max_length > 0
    bool collide = false;               // whether the two bodies still collide with each other
    f32 spring_hertz = 0;               // 0: stiff
    f32 spring_damping = 0.7f;
    bool limit = false;                 // Revolute: radians; Prismatic, Wheel: world units
    f32 lower = 0, upper = 0;
    bool motor = false;                 // Revolute, Wheel: radians per second; Prismatic, Distance: world units per second
    f32 motor_speed = 0;
    f32 max_motor_force = 0;            // torque for Revolute and Wheel
};
BOWL_COMPONENT(Joint, kind, a, b, anchor_a, anchor_b, axis, length, min_length, max_length, collide, spring_hertz,
             spring_damping, limit, lower, upper, motor, motor_speed, max_motor_force)

// What touched what during the last step_physics: a world resource, replaced every step, in a stable order.
struct ContactEvents {
    struct Touch {
        ecs::Entity a, b;
        Vec2 point, normal; // began only: where they touch, and the direction from a to b
    };
    struct Hit {
        ecs::Entity a, b;
        Vec2 point, normal;
        f32 speed; // how fast they met, world units per second
    };
    std::vector<Touch> began, ended;
    std::vector<Hit> hits; // impacts faster than PhysicsSettings::hit_speed: sounds, sparks, damage
};

// Brings Box2D up to date with the world (new and removed bodies and joints, positions and velocities the game
// changed, changed colliders), steps it by dt, writes positions, rotations and velocities back, and fills
// ContactEvents.
void step_physics(ecs::World& world, f32 dt);

// Pushes a body: an instant change in momentum, in world units.
void apply_impulse(ecs::World& world, ecs::Entity e, Vec2 impulse);

} // namespace bowl::physics
