#include "ph/gfx/sprites.hpp"

#include <algorithm>
#include <cmath>

namespace ph::gfx {

// --- Camera2D -------------------------------------------------------------------------------------------------------

Mat4 Camera2D::view_projection() const {
    Vec2 pos = position;
    if (pixel_snap) pos = {std::round(pos.x * zoom) / zoom, std::round(pos.y * zoom) / zoom};
    f32 hw = viewport.x * 0.5f, hh = viewport.y * 0.5f;
    // y-down world: the top of the screen (negative y) maps to +1 in clip space.
    Mat4 projection = Mat4::ortho(-hw, hw, hh, -hh);
    Mat4 view = Mat4::scale({zoom, zoom, 1}) * Mat4::rotation_z(-rotation) * Mat4::translation({-pos.x, -pos.y, 0});
    return projection * view;
}

Vec2 Camera2D::screen_to_world(Vec2 screen) const {
    Vec2 v = screen - viewport * 0.5f;
    return position + (v / zoom).rotated(rotation);
}

Vec2 Camera2D::world_to_screen(Vec2 world) const { return (world - position).rotated(-rotation) * zoom + viewport * 0.5f; }

Rect Camera2D::visible_bounds() const {
    Vec2 corners[4] = {screen_to_world({0, 0}), screen_to_world({viewport.x, 0}), screen_to_world({0, viewport.y}),
                       screen_to_world(viewport)};
    Vec2 lo = corners[0], hi = corners[0];
    for (Vec2 c : corners) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y)};
    }
    return {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

// --- SpriteBatch ----------------------------------------------------------------------------------------------------

void SpriteBatch::begin(const Camera2D& camera, SpriteSort sort) {
    camera_ = camera;
    sort_ = sort;
    sprites_.clear();
    textures_.clear();
}

void SpriteBatch::draw(const Texture& texture, const SpriteInstance& sprite) {
    sprites_.push_back(sprite);
    textures_.push_back(&texture);
}

void SpriteBatch::draw(const Texture& texture, Vec2 position, Color color, f32 depth) {
    SpriteInstance s;
    s.position = position;
    s.size = texture.size();
    s.color = color;
    s.depth = depth;
    draw(texture, s);
}

void SpriteBatch::draw_region(const Texture& texture, Rect source, Vec2 position, Vec2 size, Color color, f32 depth,
                              f32 rotation, Vec2 origin) {
    Vec2 inv{1.0f / static_cast<f32>(texture.width()), 1.0f / static_cast<f32>(texture.height())};
    SpriteInstance s;
    s.position = position;
    s.size = size;
    s.origin = origin;
    s.rotation = rotation;
    s.depth = depth;
    s.uv = {source.x * inv.x, source.y * inv.y, source.right() * inv.x, source.bottom() * inv.y};
    s.color = color;
    draw(texture, s);
}

void SpriteBatch::draw_rect(Rect rect, Color color, f32 depth) {
    SpriteInstance s;
    s.position = rect.position();
    s.size = rect.size();
    s.origin = {0, 0};
    s.depth = depth;
    s.color = color;
    sprites_.push_back(s);
    textures_.push_back(nullptr);
}

} // namespace ph::gfx
