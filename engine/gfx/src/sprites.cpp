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

void SpriteBatch::begin(const Camera2D& camera, SpriteSort sort, BlendMode blend, const Material* material) {
    camera_ = camera;
    sort_ = sort;
    blend_ = blend;
    material_ = material;
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

void SpriteBatch::draw_outline(Rect r, Color color, f32 t, f32 depth) {
    draw_rect({r.x, r.y, r.w, t}, color, depth);
    draw_rect({r.x, r.bottom() - t, r.w, t}, color, depth);
    draw_rect({r.x, r.y + t, t, r.h - 2 * t}, color, depth);
    draw_rect({r.right() - t, r.y + t, t, r.h - 2 * t}, color, depth);
}

void SpriteBatch::draw_nine_slice(const Texture& texture, Rect dest, Vec4 insets, Color color, f32 depth) {
    f32 tw = static_cast<f32>(texture.width()), th = static_cast<f32>(texture.height());
    f32 l = insets.x, t = insets.y, r = insets.z, b = insets.w;
    // Shrink the borders if the destination is smaller than both borders together.
    f32 sx = std::min(1.0f, dest.w / std::max(l + r, 1e-3f)), sy = std::min(1.0f, dest.h / std::max(t + b, 1e-3f));
    f32 dl = l * sx, dr = r * sx, dt = t * sy, db = b * sy;
    f32 src_x[4] = {0, l, tw - r, tw}, src_y[4] = {0, t, th - b, th};
    f32 dst_x[4] = {dest.x, dest.x + dl, dest.right() - dr, dest.right()};
    f32 dst_y[4] = {dest.y, dest.y + dt, dest.bottom() - db, dest.bottom()};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            Rect src{src_x[col], src_y[row], src_x[col + 1] - src_x[col], src_y[row + 1] - src_y[row]};
            Vec2 at{dst_x[col], dst_y[row]};
            Vec2 size{dst_x[col + 1] - dst_x[col], dst_y[row + 1] - dst_y[row]};
            if (size.x <= 0 || size.y <= 0 || src.w <= 0 || src.h <= 0) continue;
            draw_region(texture, src, at, size, color, depth, 0, {0, 0});
        }
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
