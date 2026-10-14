#pragma once

#include "ph/gfx/sprites.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ph::gfx {

// A TrueType/OpenType font. Glyphs are rasterized on first use, at the exact pixel size requested, into shared atlas
// pages, so text is sharp at any size without preprocessing. Loaded as an asset from .ttf/.otf files.
class Font : NonCopyable {
public:
    struct Glyph {
        u16 page = 0;
        Rect source;    // pixels within the page
        Vec2 offset;    // from the pen position on the baseline to the bitmap's top-left, in pixels
        f32 advance = 0;
        bool visible = false; // false for whitespace
    };
    struct Metrics {
        f32 ascent, descent, line_gap; // pixels at the given size; descent is negative
        f32 line_height() const { return ascent - descent + line_gap; }
    };

    // device may be null for layout-only use (measuring, tests); pages then never reach the GPU.
    static std::unique_ptr<Font> from_memory(GpuDevice* device, std::vector<u8> ttf, std::string_view name);
    ~Font();

    // Glyph for a code point at a pixel size (rounded to whole pixels). Missing glyphs fall back to '?'.
    const Glyph& glyph(u32 codepoint, f32 pixel_size);
    f32 kerning(u32 left, u32 right, f32 pixel_size) const;
    Metrics metrics(f32 pixel_size) const;

    // The atlas page as a texture, uploaded if new glyphs were added since the last call.
    const Texture* page(u16 index);
    usize page_count() const { return pages_.size(); }
    static constexpr u32 page_size = 1024;

private:
    Font() = default;
    struct Page {
        std::vector<u8> pixels; // RGBA8 premultiplied (white glyphs)
        std::unique_ptr<Texture> texture;
        u32 shelf_x = 1, shelf_y = 1, shelf_h = 0;
        // Bounds of glyphs added since the last upload (empty when the GPU copy is current).
        u32 dirty_x0 = page_size, dirty_y0 = page_size, dirty_x1 = 0, dirty_y1 = 0;
        bool dirty() const { return dirty_x1 > dirty_x0; }
        void mark(u32 x, u32 y, u32 w, u32 h) {
            dirty_x0 = std::min(dirty_x0, x);
            dirty_y0 = std::min(dirty_y0, y);
            dirty_x1 = std::max(dirty_x1, x + w);
            dirty_y1 = std::max(dirty_y1, y + h);
        }
    };
    bool place(u32 w, u32 h, u16& page, u32& x, u32& y);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    GpuDevice* device_ = nullptr;
    std::string name_;
    std::vector<Page> pages_;
    std::unordered_map<u64, Glyph> glyphs_; // key: size_px << 32 | codepoint
};

enum class TextAlign : u8 { Left, Center, Right };

struct TextStyle {
    f32 size = 16;            // font size (ascent to descent) in world units; logical points for UI
    Color color = Color::white();
    TextAlign align = TextAlign::Left;
    f32 max_width = 0;        // > 0: wrap words at this width
    f32 line_spacing = 1;     // multiplier on the font's line height
    // How many pixels one world unit covers, so glyphs rasterize at screen resolution. For a UI camera on a high-DPI
    // window use window.pixel_density(); for world text multiply by the camera zoom.
    f32 raster_scale = 1;
    // Draw only the first N visible characters, with the layout of the full text (typewriter reveals). <0 = all.
    i32 max_glyphs = -1;
};

// Draws UTF-8 text with its top-left (or top-center/right, per align) at position. '\n' breaks lines.
// Returns the size of the laid-out block.
Vec2 draw_text(SpriteBatch& batch, Font& font, std::string_view text, Vec2 position, const TextStyle& style,
               f32 depth = 0);
Vec2 measure_text(Font& font, std::string_view text, const TextStyle& style);

} // namespace ph::gfx
