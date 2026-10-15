# Collision

`ph/physics/collision.hpp` has the collision tools most 2D games need: overlap tests that say how to separate,
swept boxes that never pass through walls, raycasts, a broadphase for many moving things, and `move_and_slide` for
characters. It isn't a rigid-body physics engine: no forces, joints or stacking. That's deliberate, since most 2D
games want precise, predictable movement more than simulated physics.

Everything works in any consistent unit: pixels, tiles, or the cell space of an isometric map.

```cpp
#include "ph/physics/collision.hpp"
```

## Shapes

- `Rect` (from `ph/core/math.hpp`): `{x, y, w, h}`, top-left corner and size. `Rect::from_center(center, size)` makes
  one around a point.
- `physics::Circle`: `{center, radius}`.

## Overlap and separation

```cpp
bool hit = physics::overlaps(a, b);                 // Rect/Rect, Circle/Circle, Circle/Rect
bool inside = physics::contains(circle, point);

physics::Contact c = physics::collide(player_box, wall);
if (c.hit) player_box.x += c.normal.x * c.depth, player_box.y += c.normal.y * c.depth;
```

`collide` returns the *minimum translation*: moving the first shape by `normal * depth` separates them. `normal`
points from the second shape toward the first.

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
`distance` is in world units. Rays that start inside the box don't hit it.

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

## Physics for games that want it

Rigid bodies (stacking boxes, ragdolls, vehicles) are better served by a dedicated library. Optional Box2D
integration is on the [roadmap](../roadmap.md); until then, nothing stops a game from linking Box2D itself and
syncing its bodies with components.
