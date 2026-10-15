#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/json.hpp"
#include "ph/gfx/sprites.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ph::gfx {

struct SpriteFrame {
    Rect source;        // pixels in the sheet texture (the trimmed piece)
    Vec2 offset;        // where the trimmed piece sits inside the full frame
    Vec2 size;          // the full (untrimmed) frame size
    f32 duration = 0.1f; // seconds
};

struct SpriteAnimation {
    enum class Direction : u8 { Forward, Reverse, PingPong, PingPongReverse };
    std::string name;
    u32 from = 0, to = 0; // inclusive frame range
    Direction direction = Direction::Forward;
    u32 repeat = 0;       // 0 = loop forever; otherwise play this many times, then hold the last frame
};

// Frames + named animations over one texture. Load Aseprite "Export Sprite Sheet" JSON (hash or array layout) as an
// asset, e.g. assets.load<SpriteSheet>("characters/hero.json"); its PNG loads (and hot-reloads) alongside.
class SpriteSheet {
public:
    Asset<Texture> texture;
    std::vector<SpriteFrame> frames;
    std::vector<SpriteAnimation> animations;
    Vec2 pivot{0.5f, 0.5f}; // within the full frame; (0.5, 1) puts the feet on the position

    const SpriteAnimation* find(std::string_view name) const;
    i32 index_of(std::string_view name) const;

    // Parses Aseprite JSON. `image` receives meta.image (relative to the JSON file). Returns false on bad input.
    static bool parse_aseprite(const json::Value& doc, SpriteSheet& out, std::string& image, std::string& error);
    // A uniform grid, left to right then top to bottom, as one animation named "default".
    static SpriteSheet grid(Asset<Texture> texture, u32 cell_width, u32 cell_height, u32 count, f32 frame_seconds);
};

// Plays animations from a sheet. A plain struct: use it as an ECS component and step it in a (parallel) system.
struct SpriteAnimator {
    Asset<SpriteSheet> sheet;
    u32 animation = 0;
    u32 frame = 0;        // absolute frame index into sheet->frames
    f32 time = 0;         // seconds into the current frame
    f32 speed = 1;
    u32 loops_done = 0;
    bool playing = true;
    bool finished = false;    // a non-looping animation reached its end
    bool frame_changed = false; // set for the update that changed frames (hook footsteps, hit frames, ...)
    i8 step = 1;              // current direction while ping-ponging

    // Starts an animation by name (restart=false keeps going if it's already playing).
    bool play(std::string_view name, bool restart = false);
    void update(f32 dt);
};

// Draws one frame of a sheet with its pivot at `position`. scale may be negative to flip.
void draw_frame(SpriteBatch& batch, const SpriteSheet& sheet, u32 frame, Vec2 position, Vec2 scale = {1, 1},
                Color color = Color::white(), f32 depth = 0, f32 rotation = 0);

void register_sprite_sheet_loader(AssetManager& assets);

} // namespace ph::gfx
