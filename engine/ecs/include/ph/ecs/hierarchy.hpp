#pragma once

#include "ph/core/math.hpp"
#include "ph/core/reflect.hpp"
#include "ph/ecs/schedule.hpp"
#include "ph/ecs/world.hpp"

#include <string>
#include <vector>

namespace ph::ecs {

// Parent/child transforms. Give an entity a Transform2D (relative to its parent) and the engine keeps a
// GlobalTransform2D (in the world) up to date once the transform systems run:
//
//     Entity ship = world.create(Transform2D{{100, 100}});
//     Entity turret = world.create(Transform2D{{0, -12}});
//     set_parent(world, turret, ship);   // the turret now moves and rotates with the ship
//     add_transform_systems(engine.frame_schedule());
//
// All of these components are reflected: hierarchies save, load and show in the inspector.

struct Transform2D {
    Vec2 position;
    f32 rotation = 0; // radians
    Vec2 scale{1, 1};
};
PH_COMPONENT(Transform2D, position, rotation, scale)

// Written by propagate_transforms; read it when drawing.
struct GlobalTransform2D {
    Vec2 position;
    f32 rotation = 0;
    Vec2 scale{1, 1};

    Vec2 apply(Vec2 local) const { return position + (local * scale).rotated(rotation); }
};
PH_COMPONENT(GlobalTransform2D, position, rotation, scale)

struct Parent {
    Entity entity;
};
PH_COMPONENT(Parent, entity)

struct Children {
    std::vector<Entity> list;
};
PH_COMPONENT(Children, list)

// A readable name, shown by the editor and the overlay instead of the entity's id.
struct Name {
    std::string value;
};
PH_COMPONENT(Name, value)

// Attaches child under parent (null parent: detach). Refuses (returns false) if that would make a cycle.
// Not while iterating; from systems use ctx.commands.run([=](World& w) { set_parent(w, child, parent); }).
bool set_parent(World& world, Entity child, Entity parent);
// Destroys an entity and everything below it.
void destroy_recursive(World& world, Entity root);
// Computes GlobalTransform2D for every entity with a Transform2D (adding it where missing), parents before children.
void propagate_transforms(World& world);
// Registers propagate_transforms as an exclusive system at the end of the schedule.
void add_transform_systems(Schedule& schedule);

} // namespace ph::ecs
