#include "bowl/physics/colliders.hpp"

#include <algorithm>

namespace bowl::physics {

Shape world_shape(const Collider& c, const ecs::GlobalTransform2D& t) {
    Vec2 center = t.apply(c.offset);
    f32 scale = std::max(std::abs(t.scale.x), std::abs(t.scale.y));
    switch (c.kind) {
    case Collider::Kind::Circle: return Circle{center, c.radius * scale};
    case Collider::Kind::Capsule: {
        Vec2 half = Vec2{0, c.length * 0.5f * t.scale.y}.rotated(t.rotation);
        return Capsule{center - half, center + half, c.radius * std::abs(t.scale.x)};
    }
    case Collider::Kind::Polygon: {
        Polygon p;
        for (Vec2 local : c.points) p.points.push_back(t.apply(c.offset + local));
        return p;
    }
    case Collider::Kind::Box:
    default:
        if (t.rotation == 0) return Rect::from_center(center, {c.size.x * std::abs(t.scale.x), c.size.y * std::abs(t.scale.y)});
        return Polygon::box(center, {c.size.x * std::abs(t.scale.x), c.size.y * std::abs(t.scale.y)}, t.rotation);
    }
}

bool TriggerEvents::touching(ecs::Entity trigger, ecs::Entity other) const {
    return std::binary_search(inside.begin(), inside.end(), TriggerPair{trigger, other});
}

namespace {

struct Placed {
    ecs::Entity e;
    Shape shape;
    Rect box;
    u32 layer, mask;
    bool trigger;
};

std::vector<Placed> placed_colliders(ecs::World& world) {
    std::vector<Placed> out;
    world.each<const Collider, const ecs::GlobalTransform2D>(
        [&](ecs::Entity e, const Collider& c, const ecs::GlobalTransform2D& t) {
            Shape s = world_shape(c, t);
            out.push_back({e, s, bounds(s), c.layer, c.mask, c.trigger});
        });
    return out;
}

} // namespace

void update_triggers(ecs::World& world) {
    std::vector<Placed> all = placed_colliders(world);
    SpatialHash<u32> hash(64);
    for (u32 i = 0; i < all.size(); ++i) hash.insert(i, all[i].box);

    std::vector<TriggerPair> now;
    for (const Placed& t : all) {
        if (!t.trigger) continue;
        hash.query(t.box, [&](u32 i, const Rect&) {
            const Placed& o = all[i];
            if (o.e == t.e || !(o.layer & t.mask)) return;
            if (overlaps(t.shape, o.shape)) now.push_back({t.e, o.e});
        });
    }
    std::sort(now.begin(), now.end());

    TriggerEvents* events = world.try_resource<TriggerEvents>();
    if (!events) events = &world.set_resource<TriggerEvents>();
    std::vector<TriggerPair> before = std::move(events->inside);
    events->entered.clear();
    events->exited.clear();
    std::set_difference(now.begin(), now.end(), before.begin(), before.end(), std::back_inserter(events->entered));
    std::set_difference(before.begin(), before.end(), now.begin(), now.end(), std::back_inserter(events->exited));
    events->inside = std::move(now);
}

std::vector<ecs::Entity> overlapping(ecs::World& world, const Shape& shape, u32 mask) {
    std::vector<ecs::Entity> out;
    Rect area = bounds(shape);
    for (const Placed& p : placed_colliders(world))
        if ((p.layer & mask) && p.box.overlaps(area) && overlaps(shape, p.shape)) out.push_back(p.e);
    return out;
}

} // namespace bowl::physics
