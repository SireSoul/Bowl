#include "ph/gfx/sprites.hpp"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ph::gfx {

// Camera2D

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

// SpriteBatch

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
    draw_nine_slice(texture, {0, 0, static_cast<f32>(texture.width()), static_cast<f32>(texture.height())}, dest, insets,
                    color, depth);
}

void SpriteBatch::draw_nine_slice(const Texture& texture, Rect source, Rect dest, Vec4 insets, Color color, f32 depth) {
    f32 l = insets.x, t = insets.y, r = insets.z, b = insets.w;
    // Shrink the borders if the destination is smaller than both borders together.
    f32 sx = std::min(1.0f, dest.w / std::max(l + r, 1e-3f)), sy = std::min(1.0f, dest.h / std::max(t + b, 1e-3f));
    f32 dl = l * sx, dr = r * sx, dt = t * sy, db = b * sy;
    f32 src_x[4] = {source.x, source.x + l, source.right() - r, source.right()};
    f32 src_y[4] = {source.y, source.y + t, source.bottom() - b, source.bottom()};
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

void sprite_order(const SpriteBatch& batch, std::vector<u32>& order) {
    usize n = batch.sprites_.size();
    // Sort indices, never the sprites themselves.
    order.resize(n);
    std::iota(order.begin(), order.end(), 0u);
    auto tex_id = [&](u32 i) { return batch.textures_[i] ? batch.textures_[i]->id() : 0u; };
    switch (batch.sort_) {
    case SpriteSort::None: break;
    case SpriteSort::Texture:
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return tex_id(a) < tex_id(b); });
        break;
    case SpriteSort::Depth:
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            f32 da = batch.sprites_[a].depth, db = batch.sprites_[b].depth;
            return da != db ? da < db : tex_id(a) < tex_id(b);
        });
        break;
    }
}

// StaticSprites

StaticSprites::~StaticSprites() { clear(); }

void StaticSprites::clear() {
    if (buffer_ && device_) SDL_ReleaseGPUBuffer(device_->handle(), buffer_);
    buffer_ = nullptr;
    count_ = 0;
    runs_.clear();
    bounds_ = {};
}

bool StaticSprites::build(GpuDevice& device, const SpriteBatch& batch) {
    clear();
    device_ = &device;
    std::vector<u32> order;
    sprite_order(batch, order);
    std::vector<SpriteInstance> sprites;
    sprites.reserve(order.size());
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (u32 i : order) {
        const SpriteInstance& s = batch.sprites_[i];
        const Texture* t = batch.textures_[i];
        if (!runs_.empty() && runs_.back().texture == t) ++runs_.back().count;
        else runs_.push_back({static_cast<u32>(sprites.size()), 1, t});
        sprites.push_back(s);
        // Rotation turns the quad about its pivot; the circle through its farthest corner bounds every angle.
        Vec2 lo = s.position - s.size * s.origin, hi = lo + s.size;
        if (s.rotation != 0) {
            f32 r = std::max(s.size.x * std::max(s.origin.x, 1 - s.origin.x), 0.0f);
            f32 r2 = s.size.y * std::max(s.origin.y, 1 - s.origin.y);
            f32 reach = std::sqrt(r * r + r2 * r2);
            lo = s.position - Vec2{reach, reach};
            hi = s.position + Vec2{reach, reach};
        }
        x0 = std::min(x0, lo.x);
        y0 = std::min(y0, lo.y);
        x1 = std::max(x1, hi.x);
        y1 = std::max(y1, hi.y);
    }
    count_ = static_cast<u32>(sprites.size());
    if (count_ == 0) return true;
    bounds_ = {x0, y0, x1 - x0, y1 - y0};
    u32 bytes = count_ * static_cast<u32>(sizeof(SpriteInstance));
    SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, bytes, 0};
    buffer_ = SDL_CreateGPUBuffer(device.handle(), &info);
    if (!buffer_ || !device.upload(buffer_, sprites.data(), bytes)) {
        clear();
        return false;
    }
    return true;
}

} // namespace ph::gfx
