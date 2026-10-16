#pragma once

#include "bowl/core/json.hpp"
#include "bowl/core/math.hpp"

#include <optional>
#include <string>
#include <vector>

// Packs many small pictures into one texture (an atlas), so a game draws them all with one texture: fewer draw calls,
// and one file to load. Used by the bowl-atlas tool; works on pixels in memory, so it's testable without files.
namespace bowl::atlas {

struct Image {
    std::string name; // usually the file name without extension
    u32 width = 0, height = 0;
    std::vector<u8> rgba; // width * height * 4, straight alpha
};

struct Options {
    u32 max_size = 4096; // the atlas is never wider or taller than this
    u32 padding = 1;     // transparent pixels between pictures, so filtering doesn't bleed neighbors in
    bool trim = true;    // cut off fully transparent edges (the offset is kept, so pictures still line up)
};

struct Placed {
    usize image = 0;  // index into the input
    Rect source;      // where its (trimmed) pixels are in the atlas
    Vec2 offset;      // where the trimmed part sits in the original picture
    Vec2 size;        // the original picture's size
};

struct Atlas {
    u32 width = 0, height = 0;
    std::vector<Placed> placed; // in input order
    std::vector<u8> rgba;
};

// Packs every image, choosing the smallest atlas that fits (power-of-two width). Empty with `error` set if they don't
// fit in max_size.
std::optional<Atlas> pack(const std::vector<Image>& images, const Options& options, std::string& error);

// The atlas as sprite-sheet JSON (Aseprite's "hash" layout), which gfx::SpriteSheet loads. Frames are named after the
// images. Images named like "walk_0", "walk_1" (or "walk 0") also become an animation called "walk", in number order,
// each frame lasting `frame_ms`.
json::Value sheet_json(const Atlas& atlas, const std::vector<Image>& images, std::string_view image_file,
                       u32 frame_ms = 100);

} // namespace bowl::atlas
