#pragma once

// Color glyphs (emoji) from the three kinds of color font tables: sbix (Apple's PNG strikes), CBLC/CBDT (Google's
// PNG strikes, Noto Color Emoji) and COLR/CPAL version 0 (layers of plain glyphs in palette colors, Windows' Segoe UI
// Emoji). Fonts are read with bounds checks: a broken table gives no color glyph, never a crash.

#include "ph/core/math.hpp"
#include "ph/core/types.hpp"

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ph::gfx::color {

struct Tables {
    u32 sbix = 0, cblc = 0, cbdt = 0, colr = 0, cpal = 0; // offsets in the font data; 0: none
    u32 units_per_em = 0;
    u32 glyph_count = 0;
    bool any() const { return sbix || (cblc && cbdt) || (colr && cpal); }
};
Tables find_tables(std::span<const u8> font, u32 font_start);

// A picture: premultiplied RGBA8, and where its top-left corner goes from the pen on the baseline (y down), in pixels.
struct Bitmap {
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
    Vec2 offset;
};
// The glyph's picture from sbix or CBDT, scaled to `ppem` pixels per em. Nothing if the font has none for it.
std::optional<Bitmap> bitmap_glyph(std::span<const u8> font, const Tables& tables, u32 glyph, f32 ppem);

// The glyph's COLR layers, bottom first: each a plain glyph and its color (nothing: the text color). Empty if the
// glyph has no layers.
std::vector<std::pair<u32, std::optional<Color>>> layers(std::span<const u8> font, const Tables& tables, u32 glyph);

// Resizes premultiplied RGBA8: averaged when shrinking, bilinear when growing.
std::vector<u8> resize(std::span<const u8> rgba, u32 width, u32 height, u32 new_width, u32 new_height);

} // namespace ph::gfx::color
