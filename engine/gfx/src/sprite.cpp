#include "bowl/gfx/sprite.hpp"

#include <algorithm>

namespace bowl::gfx {

Vec2 sprite_size(const Sprite& s) {
    if (s.size.x != 0 || s.size.y != 0) return s.size;
    if (s.region.w > 0 && s.region.h > 0) return s.region.size();
    if (s.texture) return s.texture->size();
    return {32, 32};
}

namespace {

SpriteInstance instance_of(const Sprite& s, const ecs::GlobalTransform2D& g) {
    SpriteInstance i;
    i.position = g.position;
    i.size = sprite_size(s) * g.scale;
    i.origin = s.origin;
    i.rotation = g.rotation;
    i.color = s.color;
    i.depth = (s.y_sort ? g.position.y : 0.0f) + s.depth;
    if (s.texture && s.region.w > 0 && s.region.h > 0) {
        Vec2 t = s.texture->size();
        i.uv = {s.region.x / t.x, s.region.y / t.y, s.region.right() / t.x, s.region.bottom() / t.y};
    }
    return i;
}

} // namespace

void draw_sprites(ecs::World& world, SpriteBatch& batch) {
    world.each<const Sprite, const ecs::GlobalTransform2D>([&](const Sprite& s, const ecs::GlobalTransform2D& g) {
        if (s.visible) batch.draw(s.texture.get(), instance_of(s, g));
    });
}

std::optional<Rect> sprite_bounds(const ecs::World& world, ecs::Entity e) {
    const Sprite* s = world.try_get<const Sprite>(e);
    const ecs::GlobalTransform2D* g = world.try_get<const ecs::GlobalTransform2D>(e);
    if (!s || !g) return std::nullopt;
    SpriteInstance i = instance_of(*s, *g);
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (Vec2 corner : {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}}) {
        Vec2 p = i.position + ((corner - i.origin) * i.size).rotated(i.rotation);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    return Rect{lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

} // namespace bowl::gfx
