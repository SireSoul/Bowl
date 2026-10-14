#pragma once

#include "ph/core/math.hpp"
#include "ph/gfx/device.hpp"

#include <vector>

namespace ph::gfx {

// GPU layout of one sprite (std430). Must match engine/assets/shaders/sprite.vert.glsl.
struct SpriteInstance {
    Vec2 position;
    Vec2 size{1, 1};
    Vec2 origin{0.5f, 0.5f}; // pivot within the sprite, 0..1
    f32 rotation = 0;         // radians
    f32 depth = 0;            // draw order: lower first (for isometric/top-down, use the y of the sprite's feet)
    Vec4 uv{0, 0, 1, 1};      // u0, v0, u1, v1
    Color color = Color::white();
};
static_assert(sizeof(SpriteInstance) == 64, "SpriteInstance must match the shader's std430 layout");

// A 2D camera with y pointing down (screen convention), measured in logical points.
struct Camera2D {
    Vec2 position;            // world point at the center of the view
    f32 zoom = 1;
    f32 rotation = 0;
    Vec2 viewport{1280, 720}; // logical size of the view; the App keeps this in sync with the window
    bool pixel_snap = false;  // round the position to whole screen pixels (crisp pixel art)

    Mat4 view_projection() const;
    Vec2 screen_to_world(Vec2 screen) const;
    Vec2 world_to_screen(Vec2 world) const;
    // World-space rectangle the camera sees (axis-aligned bound when rotated).
    Rect visible_bounds() const;
    // A camera where world coordinates equal screen coordinates (UI).
    static Camera2D screen(Vec2 viewport) { return {viewport * 0.5f, 1, 0, viewport, false}; }
};

enum class SpriteSort : u8 {
    None,         // submission order (painter's algorithm, you sort)
    Texture,      // fewest draw calls; order between textures is arbitrary
    Depth,        // by depth, then texture for equal depth — the default
};

// Collects sprites for one camera. Thousands of sprites cost one storage-buffer upload and one draw per texture
// change. Fill it every frame and hand it to Renderer::submit.
class SpriteBatch {
public:
    void begin(const Camera2D& camera, SpriteSort sort = SpriteSort::Depth);
    void draw(const Texture& texture, const SpriteInstance& sprite);
    // Convenience: a texture at its pixel size, centered at position.
    void draw(const Texture& texture, Vec2 position, Color color = Color::white(), f32 depth = 0);
    // A sub-rectangle of a texture (pixel coordinates), e.g. a sprite sheet frame.
    void draw_region(const Texture& texture, Rect source, Vec2 position, Vec2 size, Color color = Color::white(),
                     f32 depth = 0, f32 rotation = 0, Vec2 origin = {0.5f, 0.5f});
    // A filled rectangle (top-left corner at position).
    void draw_rect(Rect rect, Color color, f32 depth = 0);

    usize size() const { return sprites_.size(); }
    const Camera2D& camera() const { return camera_; }

private:
    friend class Renderer;
    Camera2D camera_;
    SpriteSort sort_ = SpriteSort::Depth;
    std::vector<SpriteInstance> sprites_;
    std::vector<const Texture*> textures_; // nullptr = the renderer's white texture
};

} // namespace ph::gfx
