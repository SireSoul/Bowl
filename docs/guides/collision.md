# Collision

`bowl/physics/collision.hpp` has the collision tools most 2D games need: overlap tests that say how to separate,
swept boxes that never pass through walls, raycasts, a broadphase for many moving things, and `move_and_slide` for
characters. `bowl/physics/colliders.hpp` puts shapes on entities and reports trigger events. For forces, stacking and
bouncing there's optional [rigid-body physics](#rigid-body-physics) through Box2D, but most 2D games want tight,
predictable movement more than a physics simulation.

Everything works in any consistent unit: pixels, tiles, or the cell space of an isometric map.

```cpp
#include "bowl/physics/collision.hpp"
```

## Shapes

- `Rect` (from `bowl/core/math.hpp`): `{x, y, w, h}`, top-left corner and size. `Rect::from_center(center, size)` makes
  one around a point.
- `physics::Circle`: `{center, radius}`.
- `physics::Capsule`: `{a, b, radius}`, a segment with thickness. Good for characters: it slides over small steps and
  corners instead of catching on them.
- `physics::Polygon`: `{points}`, any convex outline, in world coordinates. `Polygon::box(center, size, rotation)` makes
  a turned rectangle. Split concave outlines into convex pieces.
- `physics::Shape` is a `std::variant` of all four, for the generic functions below.

## Overlap and separation

```cpp
bool hit = physics::overlaps(a, b);                 // any two shapes
bool inside = physics::contains(shape, point);
Rect box = physics::bounds(shape);

physics::Contact c = physics::collide(player_box, wall);
if (c.hit) player_box.x += c.normal.x * c.depth, player_box.y += c.normal.y * c.depth;
```

`collide` returns the *minimum translation*: moving the first shape by `normal * depth` separates them. `normal`
points from the second shape toward the first. It works for every pair of shapes; rectangles and circles against each
other also have dedicated, slightly faster overloads.

## Sweeps: moving without tunneling

A fast object can jump past a thin wall between ticks if you only test where it ends up. `sweep` tests the whole
movement:

```cpp
physics::SweepHit h = physics::sweep(bullet_box, velocity * dt, wall);
if (h.hit) {
    bullet_box.x += velocity.x * dt * h.time;   // h.time: fraction of the move before contact, 0..1
    bullet_box.y += velocity.y * dt * h.time;
    // h.normal: the surface it hit
}
```

## Raycasts

```cpp
physics::RayHit r = physics::raycast(eye, direction.normalized(), 500.0f, box);
if (r.hit) { /* r.point, r.normal, r.distance */ }
```

The ray is `origin + dir * t` for `t` from 0 to `max_t`; `distance` is that `t`. Pass a normalized direction and
`distance` is in world units. Rays that start inside the shape don't hit it. `raycast` takes any `Shape`.

To cast against many boxes, test each candidate (from a spatial hash, below) and keep the nearest hit.

## move_and_slide

The function for moving characters. It moves a box by a distance and, on contact, slides along the surface instead of
stopping dead, so walking diagonally into a wall keeps moving along it. It never tunnels.

```cpp
Rect box = Rect::from_center(position, {20, 28});
physics::MoveResult m = physics::move_and_slide(box, velocity * dt, solids);
position = box.center();
if (m.collided && m.last_normal.y < 0) on_ground = true;   // the last surface faced up: a floor
```

`box` is updated in place. `MoveResult` has `moved` (how far it actually went), `collided` and `last_normal`.

`solids` tells it what's solid near the move: a function called with an area that calls `emit(rect)` for each solid
box in it. Any lambda with that shape works:

```cpp
auto walls = [&](const Rect& area, FunctionRef<void(const Rect&)> emit) {
    for (const Rect& w : level_walls)
        if (w.overlaps(area)) emit(w);
};
physics::move_and_slide(box, delta, walls);
```

### Tile grids

`physics::GridSolids` reports the solid cells of a uniform grid, which is what a tilemap needs:

```cpp
auto solid = [&](i32 x, i32 y) { return is_wall(x, y); };
physics::move_and_slide(box, delta, physics::GridSolids{{16, 16}, {0, 0}, solid});   // cell size, grid origin
```

Only cells under the area being checked are asked about, so big maps cost nothing extra. Cells outside the map are
whatever your predicate says; returning `true` there keeps characters inside.

For isometric maps, move in cell space with a cell size of 1 and convert to the screen only to draw. The isometric
sample does this:

```cpp
Rect box{p.cell.x - r, p.cell.y - r, 2 * r, 2 * r};
physics::move_and_slide(box, grid_delta, physics::GridSolids{{1, 1}, {0, 0}, solid});
p.cell = box.center();
```

## Many moving things: SpatialHash

Testing every pair of n objects is n² tests. A spatial hash buckets objects by area so each object is only tested
against its neighbors:

```cpp
physics::SpatialHash<ecs::Entity> hash(64);   // cell size: about the size of a typical object

// each tick: rebuild
hash.clear();
world.each<const Position, const Hitbox>([&](ecs::Entity e, const Position& p, const Hitbox& h) {
    hash.insert(e, Rect::from_center(p.value, h.size));
});

// then query
hash.query(explosion_area, [&](ecs::Entity e, const Rect& bounds) {
    world.deferred().add<Damage>(e, 30);
});
```

Rebuilding every tick is simpler than updating and fast enough for thousands of objects. `query` reports each item
once, even when it spans several cells. Store it as a world resource to share it between systems.

## Colliders and triggers

`physics::Collider` puts a shape on an entity. It follows the entity's `GlobalTransform2D`, so position, rotation and
scale all apply, and it saves with the world and shows in the inspector like any component.

```cpp
#include "bowl/physics/colliders.hpp"

using physics::Collider;
world.create(ecs::Transform2D{{40, 80}}, Collider{.kind = Collider::Kind::Circle, .radius = 12, .layer = 0b01});
world.create(ecs::Transform2D{{200, 80}}, Collider{.size = {64, 32}, .trigger = true, .mask = 0b01}); // a doorway
```

| Field | |
| --- | --- |
| `kind` | `Box`, `Circle`, `Capsule` or `Polygon` |
| `size` | Box width and height |
| `radius`, `length` | Circle and capsule radius; capsule length between its end centers (it stands upright) |
| `points` | Polygon outline, convex, around the entity's position |
| `offset` | From the entity's position |
| `trigger` | Reports what touches it, without blocking anything |
| `layer`, `mask` | Bits: a trigger notices a collider when `collider.layer & trigger.mask` isn't zero |

**Trigger events.** Run `update_triggers` each fixed tick, after movement and the transform systems. It fills the
world's `TriggerEvents` resource:

```cpp
fixed.add("triggers", [](ecs::SystemContext& ctx) { physics::update_triggers(ctx.world); });

const auto& events = world.resource<physics::TriggerEvents>();
for (const physics::TriggerPair& p : events.entered) { /* p.other walked into p.trigger */ }
for (const physics::TriggerPair& p : events.exited) { /* ... left it, or was destroyed */ }
bool in_shop = events.touching(shop_door, player);
```

`entered`, `inside` and `exited` are sorted, so the order is the same on every run.

**Queries.** `physics::overlapping(world, shape, mask)` returns every entity whose collider overlaps a shape: what an
explosion hits, what's under the cursor, whether a spot is free to build on. `physics::world_shape(collider,
transform)` gives a collider's shape in the world, for raycasts and your own tests.

## Rigid-body physics

For stacking, tipping over, bouncing and joints of all kinds, Bowl can drive [Box2D v3](https://box2d.org). It's
optional: configure with `-DBOWL_BOX2D=ON` (and `#if BOWL_BOX2D` in code that should build without it).

An entity with a `Transform2D`, a `Collider` (its shape) and a `RigidBody` is a body:

```cpp
#include "bowl/physics/rigid_bodies.hpp"

using physics::RigidBody;
world.create(ecs::Transform2D{{0, 400}}, Collider{.size = {800, 20}}, RigidBody{.type = RigidBody::Type::Static});
world.create(ecs::Transform2D{{0, 0}}, Collider{.size = {24, 24}}, RigidBody{.restitution = 0.3f});

fixed.add("physics", [](ecs::SystemContext& ctx) { physics::step_physics(ctx.world, ctx.dt); });
```

`step_physics` creates and removes Box2D bodies as the components come and go, pushes in changes the game made
(setting `Transform2D::position` teleports a body, setting `RigidBody::velocity` sets it moving), steps, and writes
positions, rotations and velocities back. `physics::apply_impulse(world, e, impulse)` gives a body a push.

| `RigidBody` | Default | |
| --- | --- | --- |
| `type` | `Dynamic` | `Static` never moves, `Kinematic` moves only as told, `Dynamic` is simulated |
| `density`, `friction`, `restitution` | 1, 0.6, 0 | Mass per area, grip, bounciness |
| `linear_damping`, `angular_damping` | 0 | Slowing down by itself |
| `gravity_scale` | 1 | |
| `fixed_rotation` | `false` | Never turns: characters usually want this |
| `bullet` | `false` | Extra care for fast, small things |
| `velocity`, `angular_velocity` | 0 | Read them, or set them to push |

The `PhysicsSettings` resource holds `pixels_per_meter` (32: Box2D is tuned for things around a meter in size),
`gravity` in world units per second squared (y points down) and `sub_steps`. Bodies must be top-level entities. Trigger
colliders become Box2D sensors, and `update_triggers` keeps working alongside.

### Joints

A `physics::Joint` connects two bodies. It lives on an entity of its own, so it's saved with the level and its two
body references survive loading:

```cpp
using physics::Joint;
// A door on a hinge at its left edge, swinging up to 90 degrees.
world.create(Joint{.kind = Joint::Kind::Revolute, .a = frame, .b = door, .anchor_a = {-40, 0}, .anchor_b = {-40, 0},
                   .limit = true, .lower = 0, .upper = pi / 2});
// A lamp on a rope that only pulls once it's taut.
world.create(Joint{.kind = Joint::Kind::Distance, .a = ceiling, .b = lamp, .max_length = 120});
```

| Kind | What it does | Uses |
| --- | --- | --- |
| `Distance` | Keeps the anchors `length` apart (0: as far as they are when it's made). With `max_length`, a rope | spring, motor |
| `Revolute` | A hinge at the anchors: doors, wheels, pendulums | limits (radians), spring, motor |
| `Prismatic` | A slider along `axis`, in body a's space: pistons, lifts | limits, spring, motor |
| `Weld` | Glues the two together; a spring lets it bend | spring |
| `Wheel` | A wheel on a suspension along `axis`: vehicles | suspension spring, limits, motor |

Anchors are in each body's own space, in world units. `spring_hertz` (0 is stiff) and `spring_damping` soften a joint,
`limit` with `lower`/`upper` bounds it, `motor` with `motor_speed` and `max_motor_force` drives it, and `collide` lets
the two bodies keep bumping into each other. Changing a field rebuilds the joint; destroying either body removes it.

### Contact events

After each `step_physics`, the `ContactEvents` resource says what touched what:

```cpp
const auto& contacts = world.resource<physics::ContactEvents>();
for (const auto& hit : contacts.hits)
    if (hit.speed > 300) play_at(clank, hit.point, {.gain = std::min(1.0f, hit.speed / 800)});
for (const auto& t : contacts.began) { /* t.a and t.b started touching at t.point */ }
for (const auto& t : contacts.ended) { /* stopped touching */ }
```

`hits` are impacts faster than `PhysicsSettings::hit_speed` (32 world units per second), with the point, the normal
and the speed: what sounds, sparks and fall damage need. Events come sorted by entity, so the same simulation reports
them in the same order. Triggers don't produce contacts; they keep using `update_triggers`.
