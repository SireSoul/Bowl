#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/json.hpp"
#include "ph/gfx/sprites.hpp"

#include <optional>
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

// Something that happens on a frame of an animation: a footstep, the moment a swing hits.
struct FrameEvent {
    u32 frame = 0; // counted from the animation's first frame
    std::string name;
};

struct SpriteAnimation {
    enum class Direction : u8 { Forward, Reverse, PingPong, PingPongReverse };
    std::string name;
    u32 from = 0, to = 0; // inclusive frame range
    Direction direction = Direction::Forward;
    u32 repeat = 0;       // 0 = loop forever; otherwise play this many times, then hold the last frame
    std::vector<FrameEvent> events;
};

// A named rectangle on the sprite, from Aseprite's slices: hitboxes, the stretchable middle of a nine-slice panel, an
// attachment point. It can change from frame to frame: each key applies from its frame on.
struct SpriteSlice {
    struct Key {
        u32 frame = 0;
        Rect bounds;               // pixels in the full (untrimmed) frame
        std::optional<Rect> center; // nine-slice: the part that stretches, inside bounds
        std::optional<Vec2> pivot;  // inside bounds
    };
    std::string name;
    std::string data; // Aseprite's user data text
    std::vector<Key> keys;

    // The key in effect on a frame, or null if the slice starts later.
    const Key* at(u32 frame) const;
};

// Frames + named animations over one texture. Load Aseprite "Export Sprite Sheet" JSON (hash or array layout) as an
// asset, e.g. assets.load<SpriteSheet>("characters/hero.json"); its PNG loads (and hot-reloads) alongside. Atlases made
// by ph-atlas load the same way.
class SpriteSheet {
public:
    Asset<Texture> texture;
    std::vector<SpriteFrame> frames;
    std::vector<std::string> frame_names; // as exported ("hero 0.aseprite", "tree"); may be empty
    std::vector<SpriteAnimation> animations;
    std::vector<SpriteSlice> slices;
    Vec2 pivot{0.5f, 0.5f}; // within the full frame; (0.5, 1) puts the feet on the position

    const SpriteAnimation* find(std::string_view name) const;
    i32 index_of(std::string_view name) const;
    // A frame by its exported name, with or without the file extension; -1 if there's none.
    i32 frame_index(std::string_view name) const;
    const SpriteSlice* slice(std::string_view name) const;

    // Parses Aseprite JSON. `image` receives meta.image (relative to the JSON file). Returns false on bad input.
    //
    // Animation events come from a tag's user data: "3:step 7:step" fires "step" on the tag's 4th and 8th frames
    // (counted from 0). Separate them with spaces, commas or new lines; "=" works in place of ":".
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
    bool frame_changed = false; // set for the update that changed frames
    i8 step = 1;              // current direction while ping-ponging
    u32 events_fired = 0;     // which of the animation's events the last update reached (bit per event)

    // Starts an animation by name (restart=false keeps going if it's already playing).
    bool play(std::string_view name, bool restart = false);
    void update(f32 dt);
    // Whether the last update (or play) reached a frame with this event:
    //     if (anim.fired("step")) audio.play(footstep);
    bool fired(std::string_view event) const;
};

// Draws one frame of a sheet with its pivot at `position`. scale may be negative to flip.
void draw_frame(SpriteBatch& batch, const SpriteSheet& sheet, u32 frame, Vec2 position, Vec2 scale = {1, 1},
                Color color = Color::white(), f32 depth = 0, f32 rotation = 0);

// Where a slice is in the world for a frame drawn at `position` and `scale` (as draw_frame draws it), for hitboxes
// and attachment points. Empty if the sheet has no such slice on that frame.
std::optional<Rect> slice_rect(const SpriteSheet& sheet, std::string_view slice, u32 frame, Vec2 position,
                               Vec2 scale = {1, 1});

// Draws a frame as a nine-slice panel filling `dest`: the slice's center stretches, its borders don't. Without a
// nine-slice center on that slice, draws nothing.
void draw_nine_slice(SpriteBatch& batch, const SpriteSheet& sheet, std::string_view slice, u32 frame, Rect dest,
                     Color color = Color::white(), f32 depth = 0);

void register_sprite_sheet_loader(AssetManager& assets);

} // namespace ph::gfx
