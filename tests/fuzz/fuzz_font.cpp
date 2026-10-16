// Fonts (TrueType, OpenType, collections), including color emoji tables: sbix, CBDT/CBLC and COLR/CPAL.

#include "fuzz.hpp"

#include "color_glyphs.hpp"

#include "bowl/gfx/text.hpp"

using namespace bowl;
using namespace bowl::gfx;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const u8> bytes(data, size);
    auto font = Font::from_memory(nullptr, {bytes.begin(), bytes.end()}, "fuzz");

    // The color tables on their own, for every glyph, not only the ones a character map reaches.
    color::Tables tables = color::find_tables(bytes, 0);
    for (u32 glyph = 0; glyph < std::min(tables.glyph_count, 8u); ++glyph) {
        color::bitmap_glyph(bytes, tables, glyph, 24);
        color::layers(bytes, tables, glyph);
    }
    if (!font) return 0;

    // What drawing text does: characters at a few sizes, distance fields, metrics, kerning, layout.
    for (u32 c : {u32{'A'}, u32{'g'}, u32{' '}, u32{'?'}, 0x4E2Du, 0x1F600u}) {
        font->has_glyph(c);
        font->advance(c, 17);
        font->glyph(c, 17);
        font->glyph(c, 64);
    }
    font->sdf_glyph('A');
    font->metrics(30);
    font->kerning('A', 'V', 30);
    TextStyle style;
    style.size = 20;
    layout_text(*font, "Hi \xF0\x9F\x98\x80 \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", {}, style);
    return 0;
}
