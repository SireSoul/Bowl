#pragma once

#include "bowl/ecs/hierarchy.hpp"
#include "bowl/physics/collision.hpp"

#include <vector>

namespace bowl::physics {

// A collision shape on an entity, placed by its GlobalTransform2D (position, rotation and scale all apply).
//
//     world.create(ecs::Transform2D{{40, 80}}, physics::Collider{.kind = physics::Collider::Kind::Circle, .radius = 12});
//     world.create(ecs::Transform2D{{200, 80}}, physics::Collider{.size = {64, 32}, .trigger = true}); // a doorway
//
// Triggers don't block anything; update_triggers reports what enters and leaves them. Layers decide what notices
// what: a trigger reports a collider when `collider.layer & trigger.mask` is non-zero.
struct Collider {
    enum class Kind : u8 { Box, Circle, Capsule, Polygon };
    Kind kind = Kind::Box;
    Vec2 size{16, 16};        // Box
    f32 radius = 8;           // Circle and Capsule
    f32 length = 16;          // Capsule: distance between its two end centers, standing upright
    std::vector<Vec2> points; // Polygon: convex, around the entity's position
    Vec2 offset;              // from the entity's position, before rotation
    bool trigger = false;
    u32 layer = 1;
    u32 mask = 0xFFFFFFFFu;
};
BOWL_COMPONENT(Collider, kind, size, radius, length, points, offset, trigger, layer, mask)

// The collider's shape in the world.
Shape world_shape(const Collider& collider, const ecs::GlobalTransform2D& transform);

// One entity touching a trigger.
struct TriggerPair {
    ecs::Entity trigger, other;
    bool operator==(const TriggerPair&) const = default;
    bool operator<(const TriggerPair& o) const {
        return trigger.bits() != o.trigger.bits() ? trigger.bits() < o.trigger.bits() : other.bits() < o.other.bits();
    }
};

// A world resource, refreshed by update_triggers: what started touching a trigger since the last update, what's
// still touching, and what left (or was destroyed). Sorted, so the order is the same every run.
struct TriggerEvents {
    std::vector<TriggerPair> entered, inside, exited;
    bool touching(ecs::Entity trigger, ecs::Entity other) const;
};

// Finds every collider overlapping every trigger and fills the world's TriggerEvents resource (adding it the first
// time). Run it in the fixed schedule, after movement and the transform systems:
//
//     fixed.add("triggers", [](ecs::SystemContext& ctx) { physics::update_triggers(ctx.world); });
//     for (auto [door, who] : world.resource<physics::TriggerEvents>().entered) ...
void update_triggers(ecs::World& world);

// Every entity whose collider overlaps `shape` and whose layer matches `mask`, triggers included.
std::vector<ecs::Entity> overlapping(ecs::World& world, const Shape& shape, u32 mask = 0xFFFFFFFFu);

} // namespace bowl::physics
