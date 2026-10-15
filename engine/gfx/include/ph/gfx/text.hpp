#pragma once

#include "ph/core/assets.hpp"
#include "ph/gfx/sprites.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ph::gfx {

// A TrueType/OpenType font. Glyphs are rasterized on first use, at the exact pixel size requested, into shared atlas
// pages, so text is sharp at any size without preprocessing. Loaded as an asset from .ttf/.otf files.
//
// For text that changes size smoothly (zooming, pulsing titles), glyphs can also be drawn from distance fields: see
// TextStyle::sdf.
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

    // Distance-field glyphs are made once, at this pixel size, with this many pixels of falloff around the outline.
    static constexpr f32 sdf_size = 48;
    static constexpr int sdf_spread = 6;
    static constexpr u32 page_size = 1024;

    // device may be null for layout-only use (measuring, tests); pages then never reach the GPU.
    static std::unique_ptr<Font> from_memory(GpuDevice* device, std::vector<u8> ttf, std::string_view name);
    ~Font();

    // Glyph for a code point at a pixel size (rounded to whole pixels). A character this font lacks comes from the
    // first fallback font that has it, or is drawn as '?'.
    const Glyph& glyph(u32 codepoint, f32 pixel_size);
    // The distance-field glyph, in sdf_size pixels (scale it by size / sdf_size).
    const Glyph& sdf_glyph(u32 codepoint);
    // How far the pen moves after a character, without rasterizing anything.
    f32 advance(u32 codepoint, f32 pixel_size) const;
    f32 kerning(u32 left, u32 right, f32 pixel_size) const;
    Metrics metrics(f32 pixel_size) const;
    // Whether this font, or one of its fallbacks, has the character.
    bool has_glyph(u32 codepoint) const;

    // Fonts to look in, in order, for characters this one lacks: a CJK font, an emoji font, a symbols font.
    void set_fallbacks(std::vector<Asset<Font>> fonts);
    const std::vector<Asset<Font>>& fallbacks() const { return fallbacks_; }

    // The atlas pages as textures, uploaded if new glyphs were added since the last call.
    const Texture* page(u16 index);
    const Texture* sdf_page(u16 index);
    usize page_count() const { return pages_.size(); }

private:
    Font() = default;
    struct Page {
        std::vector<u8> pixels; // RGBA8 premultiplied (white glyphs), or distances in alpha for SDF pages
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
    struct Impl;
    struct Face; // which font file draws a character, and its glyph index there
    Face face_for(u32 codepoint) const;
    static bool place(std::vector<Page>& pages, u32 w, u32 h, u16& page, u32& x, u32& y);
    const Texture* upload(std::vector<Page>& pages, u16 index, std::string_view label);

    std::unique_ptr<Impl> impl_;
    GpuDevice* device_ = nullptr;
    std::string name_;
    std::vector<Page> pages_, sdf_pages_;
    std::unordered_map<u64, Glyph> glyphs_; // key: size_px << 32 | codepoint
    std::unordered_map<u32, Glyph> sdf_glyphs_;
    std::vector<Asset<Font>> fallbacks_;
};

enum class TextAlign : u8 { Left, Center, Right };

struct TextStyle {
    f32 size = 16;            // font size (ascent to descent) in world units; logical points for UI
    Color color = Color::white();
    TextAlign align = TextAlign::Left;
    f32 max_width = 0;        // > 0: wrap words at this width
    f32 line_spacing = 1;     // multiplier on the font's line height
    // How many pixels one world unit covers, so glyphs rasterize at screen resolution. For a UI camera on a high-DPI
    // window use window.pixel_density(); for world text multiply by the camera zoom. Not needed with sdf.
    f32 raster_scale = 1;
    // Draw only the first N visible characters, with the layout of the full text (typewriter reveals). <0 = all.
    i32 max_glyphs = -1;

    // An outline around each letter, `outline` world units thick (up to a few), drawn under the text.
    f32 outline = 0;
    Color outline_color = Color::black();
    // A copy of the text under it, moved by `shadow` (0, 0: none).
    Vec2 shadow;
    Color shadow_color = Color::black().with_alpha(0.6f);

    // Read [color=#ffcc00]...[/color], [wave]...[/wave] and [shake]...[/shake] in the text ("[[" is a "["). Effects
    // move with `time` (seconds; pass engine.time()).
    bool markup = false;
    f32 time = 0;

    // Draw from distance fields: sharp at any size and rotation without re-rasterizing. The batch must be begun with
    // sdf_text_material(), which also draws SDF outlines (style.outline is ignored here).
    bool sdf = false;
};

// One laid-out character, for drawing text your own way.
struct PlacedGlyph {
    u32 codepoint = 0;
    u32 index = 0;      // among the visible characters, from 0
    u16 page = 0;
    bool sdf = false;
    Rect source;        // in the font page
    Vec2 position;      // top-left corner in the world
    Vec2 size;          // in the world
    Color color;
};

// Draws UTF-8 text with its top-left (or top-center/right, per align) at position. '\n' breaks lines.
// Returns the size of the laid-out block.
Vec2 draw_text(SpriteBatch& batch, Font& font, std::string_view text, Vec2 position, const TextStyle& style,
               f32 depth = 0);
Vec2 measure_text(Font& font, std::string_view text, const TextStyle& style);
// The characters draw_text would draw (shadows and outlines aside), in order.
std::vector<PlacedGlyph> layout_text(Font& font, std::string_view text, Vec2 position, const TextStyle& style);
// The text with markup tags taken out.
std::string strip_markup(std::string_view text);

// How distance-field text looks: the outline is in parts of the glyph's falloff (0 to about 0.4), softness blurs the
// edge (0 is crisp).
struct SdfTextLook {
    Color outline_color = Color::black();
    f32 outline = 0;
    f32 softness = 0;
};
// A material for batches of SDF text. Keep it alive while the batch is used.
Material sdf_text_material(AssetManager& assets, const SdfTextLook& look = {});

} // namespace ph::gfx
