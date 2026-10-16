#pragma once

// Fonts read with stb_truetype, behind checks. stb follows every offset and count in a font file without checking
// any of them, so a broken or hostile font (from a mod, a download, a player's own folder) could make it read far
// outside the file, recurse until the stack runs out, or work for hours. Everything stb reads is checked here first:
// the tables when the font loads, and each glyph's outline before stb draws it. A font that fails is refused; a glyph
// that fails is drawn as nothing. Character lookup and kerning are done here, with checked reads, instead of by stb.

#include "bowl/core/types.hpp"

#include <optional>
#include <span>
#include <unordered_map>

#include <stb_truetype.h> // declarations only: the implementation is in truetype.cpp

namespace bowl::gfx::truetype {

// Where the first font of the file starts: 0, or its offset in a collection (.ttc). Nothing if it isn't a font.
std::optional<u32> font_start(std::span<const u8> data);

class Font {
  public:
    // False if the data isn't a font stb can read safely. `bitmap_glyphs`: the font has color pictures (sbix, CBDT),
    // so it may have no outlines at all, like Noto Color Emoji. `data` must stay alive and unchanged.
    bool load(std::span<const u8> data, u32 start, bool bitmap_glyphs);

    u32 glyph_count() const { return glyph_count_; }
    // The glyph for a character, from the character map stb would choose; 0 if there's none.
    int glyph_for(u32 codepoint) const;
    // Kerning between two glyphs in font units, from GPOS or kern as stb reads them.
    int kerning(int left, int right) const;

    // stb's metrics and drawing. Glyphs that fail their checks have no outline: an empty box, nothing drawn.
    f32 scale_for_pixel_height(f32 pixels) const { return stbtt_ScaleForPixelHeight(&info_, pixels); }
    void vertical_metrics(int& ascent, int& descent, int& line_gap) const;
    void horizontal_metrics(int glyph, int& advance, int& left_bearing) const;
    void bitmap_box(int glyph, f32 scale, int& x0, int& y0, int& x1, int& y1) const;
    void make_bitmap(u8* out, int width, int height, int stride, f32 scale, int glyph) const;
    // A distance field (stbtt_GetGlyphSDF; free it with stbtt_FreeSDF), or null. Null too for a glyph so big and
    // intricate that making it would take seconds.
    u8* sdf(f32 scale, int glyph, int padding, u8 on_edge, f32 pixel_dist_scale, int& width, int& height, int& x_offset,
            int& y_offset) const;

  private:
    // What reading a glyph's outline involves, worked out without letting stb near it.
    struct Outline {
        enum class State : u8 { Checking, Good, Bad } state = State::Checking;
        u32 vertices = 0; // how many stb makes
        u32 visits = 0;   // glyphs stb reads to make them (composites read their parts, each time they're used)
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; // extent of every coordinate, and 0
    };
    struct Table {
        u32 offset = 0, length = 0;
        explicit operator bool() const { return offset != 0; }
        std::span<const u8> in(std::span<const u8> data) const { return data.subspan(offset, length); }
    };

    bool outline(int glyph) const;
    const Outline& check_glyf(u32 glyph, int depth) const;
    bool check_charstring(int glyph, u32& vertices) const;
    bool load_cff(Table cff);
    int gpos_kerning(int left, int right) const;
    int kern_kerning(int left, int right) const;

    std::span<const u8> data_;
    stbtt_fontinfo info_{};
    u32 glyph_count_ = 0;
    Table cmap_; // the character map subtable, to the end of the cmap table
    Table glyf_, loca_, kern_, gpos_;
    mutable std::unordered_map<u32, Outline> outlines_;
};

} // namespace bowl::gfx::truetype
