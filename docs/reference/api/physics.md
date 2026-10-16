# physics

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/physics/colliders.hpp](#bowlphysicscollidershpp)
- [bowl/physics/collision.hpp](#bowlphysicscollisionhpp)
- [bowl/physics/rigid_bodies.hpp](#bowlphysicsrigid_bodieshpp)

## bowl/physics/colliders.hpp

`#include "bowl/physics/colliders.hpp"` · namespace `bowl::physics`

### Collider

A collision shape on an entity, placed by its GlobalTransform2D (position, rotation and scale all apply).

```cpp
world.create(ecs::Transform2D{{40, 80}}, physics::Collider{.kind = physics::Collider::Kind::Circle, .radius = 12});
world.create(ecs::Transform2D{{200, 80}}, physics::Collider{.size = {64, 32}, .trigger = true}); // a doorway
```
Triggers don't block anything; update_triggers reports what enters and leaves them. Layers decide what notices
what: a trigger reports a collider when `collider.layer & trigger.mask` is non-zero.

```cpp
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
```

```cpp
BOWL_COMPONENT(Collider, kind, size, radius, length, points, offset, trigger, layer, mask)
```

The collider's shape in the world.

```cpp
Shape world_shape(const Collider& collider, const ecs::GlobalTransform2D& transform);
```

### TriggerPair

One entity touching a trigger.

```cpp
struct TriggerPair
```

```cpp
ecs::Entity trigger, other;
bool operator==(const TriggerPair&) const = default;
bool operator<(const TriggerPair& o) const;
```

### TriggerEvents

A world resource, refreshed by update_triggers: what started touching a trigger since the last update, what's
still touching, and what left (or was destroyed). Sorted, so the order is the same every run.

```cpp
struct TriggerEvents
```

```cpp
std::vector<TriggerPair> entered, inside, exited;
bool touching(ecs::Entity trigger, ecs::Entity other) const;
```

Finds every collider overlapping every trigger and fills the world's TriggerEvents resource (adding it the first
time). Run it in the fixed schedule, after movement and the transform systems:

```cpp
fixed.add("triggers", [](ecs::SystemContext& ctx) { physics::update_triggers(ctx.world); });
for (auto [door, who] : world.resource<physics::TriggerEvents>().entered) ...
```

```cpp
void update_triggers(ecs::World& world);
```

Every entity whose collider overlaps `shape` and whose layer matches `mask`, triggers included.

```cpp
std::vector<ecs::Entity> overlapping(ecs::World& world, const Shape& shape, u32 mask = 0xFFFFFFFFu);
```

## bowl/physics/collision.hpp

`#include "bowl/physics/collision.hpp"` · namespace `bowl::physics`

Collision for games that move things around a world: overlap tests that say how to separate, swept boxes that
never tunnel, raycasts, a spatial hash broadphase, and move_and_slide for characters. Shapes are rectangles,
circles, capsules and convex polygons. Any consistent unit works: pixels, tiles, or the cell space of an isometric
map. For colliders on entities and trigger events, see colliders.hpp.

### Circle

```cpp
struct Circle {
    Vec2 center;
    f32 radius = 0;
};
```

### Contact

Minimum translation: move the first shape by normal * depth to stop overlapping.

```cpp
struct Contact {
    bool hit = false;
    Vec2 normal; // points from the second shape toward the first
    f32 depth = 0;
};
```

```cpp
bool overlaps(const Rect& a, const Rect& b);
bool overlaps(const Circle& a, const Circle& b);
bool overlaps(const Circle& c, const Rect& r);
bool contains(const Circle& c, Vec2 p);
```

```cpp
Contact collide(const Rect& a, const Rect& b);
Contact collide(const Circle& a, const Circle& b);
Contact collide(const Circle& c, const Rect& r);
```

### SweepHit

A box moving by `delta` against a static box. time is the fraction of delta travelled before touching (0..1).

```cpp
struct SweepHit {
    bool hit = false;
    f32 time = 1;
    Vec2 normal;
};
```

```cpp
SweepHit sweep(const Rect& moving, Vec2 delta, const Rect& target);
```

### RayHit

```cpp
struct RayHit {
    bool hit = false;
    f32 distance = 0;
    Vec2 point, normal;
};
```

Finds where origin + dir * t first enters the box, for t in [0, max_t]. `distance` is that t (in units of dir's
length; pass a normalized dir to get world distance). Rays starting inside the box don't hit it.

```cpp
RayHit raycast(Vec2 origin, Vec2 dir, f32 max_t, const Rect& box);
```

### Polygon

A convex polygon, in world coordinates. Points go around the outline in either direction; a concave outline needs
splitting into convex pieces.

```cpp
struct Polygon
```

```cpp
std::vector<Vec2> points;
```

A rectangle, optionally turned about its center.

```cpp
static Polygon box(Vec2 center, Vec2 size, f32 rotation = 0);
static Polygon box(const Rect& r);
Rect bounds() const;
```

### Capsule

A line segment with thickness: two circles joined by a rectangle. Good for characters (smooth over steps and
corners) and long thin things.

```cpp
struct Capsule
```

```cpp
Vec2 a, b;
f32 radius = 0;
Rect bounds() const;
```

Any of the shapes, for the generic tests below.

```cpp
using Shape = std::variant<Rect, Circle, Capsule, Polygon>;
```

```cpp
Rect bounds(const Shape& shape);
bool overlaps(const Shape& a, const Shape& b);
bool contains(const Shape& shape, Vec2 p);
```

The minimum translation that separates a from b, for any pair of shapes.

```cpp
Contact collide(const Shape& a, const Shape& b);
RayHit raycast(Vec2 origin, Vec2 dir, f32 max_t, const Shape& shape);
```

Supplies the solid boxes near an area: call `emit` for each.

```cpp
using SolidQuery = FunctionRef<void(const Rect& area, FunctionRef<void(const Rect&)> emit)>;
```

### GridSolids

Solid cells of a uniform grid (tilemaps): solid(x, y) decides per cell; cells are cell_size wide, starting at origin.

```cpp
struct GridSolids
```

```cpp
Vec2 cell_size{1, 1};
Vec2 origin;
FunctionRef<bool(i32 x, i32 y)> solid;
```

```cpp
void operator()(const Rect& area, FunctionRef<void(const Rect&)> emit) const;
```

### MoveResult

```cpp
struct MoveResult {
    Vec2 moved;          // how far the box actually went
    bool collided = false;
    Vec2 last_normal;    // normal of the last surface touched (e.g. normal.y < 0 means standing on a floor)
};
```

Moves `box` by `delta`, sliding along whatever `solids` reports instead of stopping dead, without tunneling.
Updates box in place.

```cpp
MoveResult move_and_slide(Rect& box, Vec2 delta, SolidQuery solids, int max_iterations = 4);
```

### SpatialHash

Uniform-grid spatial hash for broadphase over many moving things. Rebuild it every tick (clear + insert): that is
cheaper than updating and stays correct. T is usually an ecs::Entity.

```cpp
template <class T> class SpatialHash
```

```cpp
explicit SpatialHash(f32 cell_size = 64);
```

```cpp
void clear();
void insert(const T& value, const Rect& bounds);
```

Calls f(value, bounds) once for each item whose bounds overlap `area`.

```cpp
template <class F> void query(const Rect& area, F&& f);
usize size() const;
```

## bowl/physics/rigid_bodies.hpp

`#include "bowl/physics/rigid_bodies.hpp"` · namespace `bowl::physics`

Rigid-body physics through Box2D v3: gravity, stacking, bouncing, things that tip over. Optional: build with
-DBOWL_BOX2D=ON (BOWL_BOX2D is then 1 in code). Most games don't need it: move_and_slide and triggers cover precise
character movement better.

A body is an entity with a Transform2D, a Collider (its shape) and a RigidBody:

```cpp
world.create(ecs::Transform2D{{0, 400}}, physics::Collider{.size = {800, 20}},
             physics::RigidBody{.type = physics::RigidBody::Type::Static});   // the floor
world.create(ecs::Transform2D{{0, 0}}, physics::Collider{.size = {24, 24}}, physics::RigidBody{});  // a crate

fixed.add("physics", [](ecs::SystemContext& ctx) { physics::step_physics(ctx.world, ctx.dt); });
```
Bodies must be top-level entities (no Parent). Positions are in world units; PhysicsSettings::pixels_per_meter
says how many make a meter, which is what Box2D's tuning (and the default gravity) is in.

### RigidBody

```cpp
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
```

```cpp
BOWL_COMPONENT(RigidBody, type, density, friction, restitution, linear_damping, angular_damping, gravity_scale,
             fixed_rotation, bullet, velocity, angular_velocity)
```

### PhysicsSettings

A world resource; added with defaults by the first step_physics.

```cpp
struct PhysicsSettings {
    f32 pixels_per_meter = 32;
    Vec2 gravity{0, 9.81f * 32}; // world units per second squared; y points down
    int sub_steps = 4;
    f32 hit_speed = 32;          // ContactEvents::hits only reports impacts faster than this (world units per second)
};
```

### Joint

A joint between two bodies, on an entity of its own (it's saved and loaded with the world like anything else, and
the two body references survive). Anchors are in each body's own space, in world units. What each kind does:
  Distance   keeps the anchors `length` apart: a rod, a bungee with a spring, a rope with max_length
  Revolute   a hinge at the anchors: doors, wheels, pendulums; limits in radians, a motor turns it
  Prismatic  a slider along `axis` (in body a's space): pistons, elevators; limits and motor along the axis
  Weld       glues two bodies; a spring lets it bend (wobbly bridges)
  Wheel      a wheel on a suspension along `axis`: cars; the spring is the suspension, the motor drives it
Changing a field rebuilds the joint. Removing the component, or destroying either body, removes it.

```cpp
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
```

```cpp
BOWL_COMPONENT(Joint, kind, a, b, anchor_a, anchor_b, axis, length, min_length, max_length, collide, spring_hertz,
             spring_damping, limit, lower, upper, motor, motor_speed, max_motor_force)
```

### ContactEvents

What touched what during the last step_physics: a world resource, replaced every step, in a stable order.

```cpp
struct ContactEvents
```

#### Touch

```cpp
struct Touch {
    ecs::Entity a, b;
    Vec2 point, normal; // began only: where they touch, and the direction from a to b
};
```

#### Hit

```cpp
struct Hit {
    ecs::Entity a, b;
    Vec2 point, normal;
    f32 speed; // how fast they met, world units per second
};
```

```cpp
std::vector<Touch> began, ended;
std::vector<Hit> hits; // impacts faster than PhysicsSettings::hit_speed: sounds, sparks, damage
```

Brings Box2D up to date with the world (new and removed bodies and joints, positions and velocities the game
changed, changed colliders), steps it by dt, writes positions, rotations and velocities back, and fills
ContactEvents.

```cpp
void step_physics(ecs::World& world, f32 dt);
```

Pushes a body: an instant change in momentum, in world units.

```cpp
void apply_impulse(ecs::World& world, ecs::Entity e, Vec2 impulse);
```

