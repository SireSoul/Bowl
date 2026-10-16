#include "ph/core/file.hpp"
#include "ph/core/utf8.hpp"
#include "ph/gfx/text.hpp"
#include "ph/icon/icon.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>

using namespace ph;

TEST_CASE("utf8: decode and encode") {
    std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"; // a é € 😀
    usize i = 0;
    CHECK(utf8::next(s, i) == 'a');
    CHECK(utf8::next(s, i) == 0xE9);
    CHECK(utf8::next(s, i) == 0x20AC);
    CHECK(utf8::next(s, i) == 0x1F600);
    CHECK(i == s.size());

    std::string out;
    for (u32 cp : {u32('a'), 0xE9u, 0x20ACu, 0x1F600u}) utf8::append(out, cp);
    CHECK(out == s);
}

TEST_CASE("utf8: malformed input decodes as U+FFFD and always advances") {
    for (std::string bad : {std::string("\xFF"), std::string("\xC3"), std::string("\xE2\x82"), std::string("\xC0\xAF"),
                            std::string("\xED\xA0\x80")}) {
        usize i = 0;
        CHECK(utf8::next(bad, i) == utf8::replacement);
        CHECK(i == 1);
    }
}

namespace {
std::unique_ptr<gfx::Font> engine_font() {
    auto bytes = read_file_bytes(std::filesystem::path(PH_ENGINE_ASSET_SOURCE_DIR) / "fonts" / "Roboto-Medium.ttf");
    REQUIRE(bytes);
    auto font = gfx::Font::from_memory(nullptr, std::move(*bytes), "Roboto");
    REQUIRE(font);
    return font;
}
} // namespace

TEST_CASE("text: glyphs, metrics and fallback") {
    auto font = engine_font();
    const auto& a = font->glyph('A', 32);
    CHECK(a.visible);
    CHECK(a.advance > 10);
    CHECK(a.source.h > 15);
    CHECK_FALSE(font->glyph(' ', 32).visible);
    CHECK(font->glyph(' ', 32).advance > 0);
    // Not in Roboto: falls back to '?'
    CHECK(font->glyph(0x4E2D, 32).advance == doctest::Approx(font->glyph('?', 32).advance));
    auto m = font->metrics(32);
    CHECK(m.ascent - m.descent == doctest::Approx(32).epsilon(0.05));
}

TEST_CASE("text: measuring, newlines and word wrap") {
    auto font = engine_font();
    gfx::TextStyle style;
    style.size = 20;
    Vec2 one = gfx::measure_text(*font, "hello world", style);
    Vec2 two = gfx::measure_text(*font, "hello\nworld", style);
    CHECK(one.x > two.x);
    CHECK(two.y == doctest::Approx(one.y * 2));

    style.max_width = one.x * 0.6f; // forces "world" onto its own line
    Vec2 wrapped = gfx::measure_text(*font, "hello world", style);
    CHECK(wrapped.y == doctest::Approx(two.y));
    CHECK(wrapped.x <= style.max_width);

    // Raster scale changes resolution, not layout size (within rounding).
    gfx::TextStyle hi = style;
    hi.raster_scale = 2;
    CHECK(gfx::measure_text(*font, "hello world", hi).x == doctest::Approx(wrapped.x).epsilon(0.08));
}

TEST_CASE("text: markup is taken out of the text and colors the characters") {
    CHECK(gfx::strip_markup("a [color=#ff0000]red[/color] [[b] [wave]w[/wave]") == "a red [b] w");
    CHECK(gfx::strip_markup("[nope] stays") == "[nope] stays");

    auto font = engine_font();
    gfx::TextStyle style;
    style.markup = true;
    auto glyphs = gfx::layout_text(*font, "a[color=#00ff00]bc[/color]d", {}, style);
    REQUIRE(glyphs.size() == 4);
    CHECK(glyphs[0].color == Color::white());
    CHECK(glyphs[1].color == Color::hex(0x00FF00));
    CHECK(glyphs[2].color == Color::hex(0x00FF00));
    CHECK(glyphs[3].color == Color::white());

    // Tags take no space: the same width as the plain text.
    gfx::TextStyle plain;
    CHECK(gfx::measure_text(*font, "a[color=#0f0]bc[/color]d", style).x ==
          doctest::Approx(gfx::measure_text(*font, "abcd", plain).x));
}

TEST_CASE("text: wave moves letters, outlines and shadows don't change layout") {
    auto font = engine_font();
    gfx::TextStyle style;
    style.markup = true;
    style.time = 0.3f;
    auto flat = gfx::layout_text(*font, "abc", {}, style);
    auto waved = gfx::layout_text(*font, "[wave]abc[/wave]", {}, style);
    REQUIRE(flat.size() == waved.size());
    CHECK(flat[0].position.x == doctest::Approx(waved[0].position.x));
    CHECK(flat[1].position.y != doctest::Approx(waved[1].position.y));

    gfx::TextStyle outlined = style;
    outlined.outline = 2;
    outlined.shadow = {1, 1};
    CHECK(gfx::measure_text(*font, "abc", outlined).x == doctest::Approx(gfx::measure_text(*font, "abc", style).x));

    style.max_glyphs = 2;
    CHECK(gfx::layout_text(*font, "a b c", {}, style).size() == 2);
}

TEST_CASE("text: distance-field glyphs scale with the text size") {
    auto font = engine_font();
    const auto& a = font->sdf_glyph('A');
    CHECK(a.visible);
    CHECK(a.source.h > gfx::Font::sdf_size * 0.5f);

    gfx::TextStyle style;
    style.sdf = true;
    style.size = 24;
    Vec2 small = gfx::measure_text(*font, "Hello", style);
    style.size = 96;
    Vec2 big = gfx::measure_text(*font, "Hello", style);
    CHECK(big.x == doctest::Approx(small.x * 4).epsilon(0.01));
    auto glyphs = gfx::layout_text(*font, "Hi", {}, style);
    REQUIRE(glyphs.size() == 2);
    CHECK(glyphs[0].sdf);
    CHECK(glyphs[0].size.y == doctest::Approx(a.source.h * 96 / gfx::Font::sdf_size));
}

TEST_CASE("text: fallback fonts") {
    auto font = engine_font();
    CHECK(font->has_glyph('A'));
    CHECK_FALSE(font->has_glyph(0x4E2D));
    // A fallback that lacks the character too still ends at '?'.
    auto slot = std::make_shared<AssetSlot<gfx::Font>>();
    slot->value = engine_font();
    font->set_fallbacks({Asset<gfx::Font>(slot)});
    CHECK(font->fallbacks().size() == 1);
    CHECK_FALSE(font->has_glyph(0x4E2D));
    CHECK(font->glyph(0x4E2D, 20).advance == doctest::Approx(font->glyph('?', 20).advance));
}

namespace {

// A tiny TrueType font made in memory: 'A' is glyph 1 (a square), U+1F600 is glyph 2 (a color glyph, if a color
// table is given), glyph 3 is a small square COLR layers use.
struct TinyFont {
    std::vector<std::pair<std::string, std::vector<u8>>> tables;

    static void be16(std::vector<u8>& out, u32 v) { out.push_back(static_cast<u8>(v >> 8)), out.push_back(static_cast<u8>(v)); }
    static void be32(std::vector<u8>& out, u32 v) { be16(out, v >> 16), be16(out, v & 0xFFFF); }

    static std::vector<u8> square(i16 x0, i16 y0, i16 x1, i16 y1) {
        std::vector<u8> g;
        be16(g, 1); // one contour
        be16(g, static_cast<u16>(x0)), be16(g, static_cast<u16>(y0)), be16(g, static_cast<u16>(x1)), be16(g, static_cast<u16>(y1));
        be16(g, 3); // last point
        be16(g, 0); // no instructions
        for (int i = 0; i < 4; ++i) g.push_back(1); // on-curve, 16-bit deltas
        for (i32 dx : {i32{x0}, x1 - x0, 0, x0 - x1}) be16(g, static_cast<u16>(dx));
        for (i32 dy : {i32{y0}, 0, y1 - y0, 0}) be16(g, static_cast<u16>(dy));
        return g;
    }

    TinyFont() {
        std::vector<u8> head(54, 0);
        head[1] = 1;                                    // version 1.0
        head[12] = 0x5F, head[13] = 0x0F, head[14] = 0x3C, head[15] = 0xF5; // magic
        head[18] = 1000 >> 8, head[19] = 1000 & 0xFF;   // units per em
        tables.push_back({"head", head});
        std::vector<u8> hhea(36, 0);
        hhea[1] = 1;
        hhea[4] = 800 >> 8, hhea[5] = 800 & 0xFF; // ascent
        u16 descent = static_cast<u16>(-200);
        hhea[6] = static_cast<u8>(descent >> 8), hhea[7] = static_cast<u8>(descent);
        hhea[35] = 4; // horizontal metrics for 4 glyphs
        tables.push_back({"hhea", hhea});
        std::vector<u8> maxp;
        be32(maxp, 0x00005000), be16(maxp, 4);
        tables.push_back({"maxp", maxp});
        std::vector<u8> hmtx;
        for (int i = 0; i < 4; ++i) be16(hmtx, 1000), be16(hmtx, 0);
        tables.push_back({"hmtx", hmtx});
        std::vector<u8> cmap;
        be16(cmap, 0), be16(cmap, 1), be16(cmap, 3), be16(cmap, 10), be32(cmap, 12);
        be16(cmap, 12), be16(cmap, 0), be32(cmap, 16 + 2 * 12), be32(cmap, 0), be32(cmap, 2);
        be32(cmap, 'A'), be32(cmap, 'A'), be32(cmap, 1);
        be32(cmap, 0x1F600), be32(cmap, 0x1F600), be32(cmap, 2);
        tables.push_back({"cmap", cmap});
        std::vector<std::vector<u8>> glyphs = {{}, square(100, 0, 900, 800), square(100, 0, 900, 800), square(400, 300, 600, 500)};
        std::vector<u8> glyf, loca;
        for (auto& g : glyphs) {
            if (g.size() % 2) g.push_back(0);
            be16(loca, static_cast<u32>(glyf.size() / 2));
            glyf.insert(glyf.end(), g.begin(), g.end());
        }
        be16(loca, static_cast<u32>(glyf.size() / 2));
        tables.push_back({"loca", loca});
        tables.push_back({"glyf", glyf});
    }

    std::vector<u8> build() const {
        std::vector<u8> out;
        be32(out, 0x00010000), be16(out, static_cast<u32>(tables.size())), be16(out, 0), be16(out, 0), be16(out, 0);
        u32 offset = 12 + 16 * static_cast<u32>(tables.size());
        std::vector<u8> data;
        for (const auto& [tag, bytes] : tables) {
            out.insert(out.end(), tag.begin(), tag.end());
            be32(out, 0), be32(out, offset + static_cast<u32>(data.size())), be32(out, static_cast<u32>(bytes.size()));
            data.insert(data.end(), bytes.begin(), bytes.end());
            while (data.size() % 4) data.push_back(0);
        }
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }
};

// A size x size PNG of one color.
std::vector<u8> png_of(u32 size, u8 r, u8 g, u8 b) {
    icon::Image image{size, size, {}};
    for (u32 i = 0; i < size * size; ++i) image.rgba.insert(image.rgba.end(), {r, g, b, 255});
    return icon::encode_png(image);
}

// The premultiplied pixel at a point inside a glyph.
std::array<u8, 4> pixel(const gfx::Font& font, const gfx::Font::Glyph& g, f32 fx, f32 fy) {
    auto page = font.page_pixels(g.page);
    usize x = static_cast<usize>(g.source.x + g.source.w * fx), y = static_cast<usize>(g.source.y + g.source.h * fy);
    const u8* p = &page[(y * gfx::Font::page_size + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

} // namespace

TEST_CASE("text: COLR emoji are layers in palette colors") {
    TinyFont tiny;
    std::vector<u8> colr, cpal;
    TinyFont::be16(colr, 0), TinyFont::be16(colr, 1), TinyFont::be32(colr, 14), TinyFont::be32(colr, 20), TinyFont::be16(colr, 2);
    TinyFont::be16(colr, 2), TinyFont::be16(colr, 0), TinyFont::be16(colr, 2); // glyph 2: layers 0 and 1
    TinyFont::be16(colr, 1), TinyFont::be16(colr, 0);                          // the big square, red
    TinyFont::be16(colr, 3), TinyFont::be16(colr, 1);                          // the small one, blue
    TinyFont::be16(cpal, 0), TinyFont::be16(cpal, 2), TinyFont::be16(cpal, 1), TinyFont::be16(cpal, 2);
    TinyFont::be32(cpal, 14), TinyFont::be16(cpal, 0);
    cpal.insert(cpal.end(), {0, 0, 255, 255, 255, 0, 0, 255}); // BGRA: red, blue
    tiny.tables.push_back({"COLR", colr});
    tiny.tables.push_back({"CPAL", cpal});
    auto font = gfx::Font::from_memory(nullptr, tiny.build(), "tiny colr");
    REQUIRE(font);

    const auto& plain = font->glyph('A', 50);
    CHECK(plain.visible);
    CHECK_FALSE(plain.color);
    const auto& emoji = font->glyph(0x1F600, 50);
    REQUIRE(emoji.visible);
    CHECK(emoji.color);
    CHECK(emoji.source.w == doctest::Approx(40).epsilon(0.1)); // 800 units at 50 px per 1000
    auto edge = pixel(*font, emoji, 0.1f, 0.5f), middle = pixel(*font, emoji, 0.5f, 0.5f);
    CHECK(edge == std::array<u8, 4>{255, 0, 0, 255});
    CHECK(middle == std::array<u8, 4>{0, 0, 255, 255});

    // Laid out, emoji keep their colors: white, with the text's alpha.
    gfx::TextStyle style;
    style.size = 50;
    style.color = Color{1, 1, 0, 0.5f};
    auto placed = gfx::layout_text(*font, "A\xF0\x9F\x98\x80", {}, style);
    REQUIRE(placed.size() == 2);
    CHECK_FALSE(placed[0].color_glyph);
    CHECK(placed[1].color_glyph);
    CHECK(placed[1].color.r == 1);
    CHECK(placed[1].color.b == 1);
    CHECK(placed[1].color.a == 0.5f);
}

TEST_CASE("text: CBDT emoji are pictures, scaled to the text size") {
    TinyFont tiny;
    std::vector<u8> png = png_of(20, 0, 200, 0);
    std::vector<u8> cblc, cbdt;
    TinyFont::be16(cblc, 3), TinyFont::be16(cblc, 0), TinyFont::be32(cblc, 1);
    TinyFont::be32(cblc, 56), TinyFont::be32(cblc, 16), TinyFont::be32(cblc, 1), TinyFont::be32(cblc, 0);
    cblc.insert(cblc.end(), 24, 0); // line metrics
    TinyFont::be16(cblc, 2), TinyFont::be16(cblc, 2);
    cblc.insert(cblc.end(), {20, 20, 32, 1}); // ppem x, y, bit depth, flags
    TinyFont::be16(cblc, 2), TinyFont::be16(cblc, 2), TinyFont::be32(cblc, 8); // glyphs 2..2, subtable after this
    TinyFont::be16(cblc, 1), TinyFont::be16(cblc, 17), TinyFont::be32(cblc, 0); // index format 1, image format 17
    TinyFont::be32(cblc, 4), TinyFont::be32(cblc, 4 + 5 + 4 + static_cast<u32>(png.size()));
    TinyFont::be16(cbdt, 3), TinyFont::be16(cbdt, 0);
    cbdt.insert(cbdt.end(), {20, 20, 0, 16, 20}); // height, width, bearing x, bearing y, advance
    TinyFont::be32(cbdt, static_cast<u32>(png.size()));
    cbdt.insert(cbdt.end(), png.begin(), png.end());
    tiny.tables.push_back({"CBLC", cblc});
    tiny.tables.push_back({"CBDT", cbdt});
    auto font = gfx::Font::from_memory(nullptr, tiny.build(), "tiny cbdt");
    REQUIRE(font);

    const auto& big = font->glyph(0x1F600, 40); // 40 px per em: twice the strike
    REQUIRE(big.visible);
    CHECK(big.color);
    CHECK(big.source.w == 40);
    CHECK(big.source.h == 40);
    CHECK(big.offset.x == 0);
    CHECK(big.offset.y == -32); // the top is 16 strike pixels above the baseline
    CHECK(pixel(*font, big, 0.5f, 0.5f) == std::array<u8, 4>{0, 200, 0, 255});
    const auto& small = font->glyph(0x1F600, 10);
    CHECK(small.source.w == 10);
    CHECK(pixel(*font, small, 0.5f, 0.5f) == std::array<u8, 4>{0, 200, 0, 255});
}

TEST_CASE("text: sbix emoji, and fonts without color tables") {
    TinyFont tiny;
    std::vector<u8> png = png_of(20, 250, 120, 0);
    std::vector<u8> sbix;
    TinyFont::be16(sbix, 1), TinyFont::be16(sbix, 1), TinyFont::be32(sbix, 1), TinyFont::be32(sbix, 12);
    TinyFont::be16(sbix, 20), TinyFont::be16(sbix, 72); // the strike: 20 ppem
    u32 data = 4 + 4 * 5;                                // after the 5 glyph offsets
    u32 glyph2 = 8 + static_cast<u32>(png.size());
    for (u32 offset : {data, data, data, data + glyph2, data + glyph2}) TinyFont::be32(sbix, offset);
    TinyFont::be16(sbix, 0), TinyFont::be16(sbix, static_cast<u16>(-4)); // origin: the picture's bottom 4 px below
    sbix.insert(sbix.end(), {'p', 'n', 'g', ' '});
    sbix.insert(sbix.end(), png.begin(), png.end());
    tiny.tables.push_back({"sbix", sbix});
    auto font = gfx::Font::from_memory(nullptr, tiny.build(), "tiny sbix");
    REQUIRE(font);
    const auto& g = font->glyph(0x1F600, 20);
    REQUIRE(g.visible);
    CHECK(g.color);
    CHECK(g.source.w == 20);
    CHECK(g.offset.y == -16);
    CHECK(pixel(*font, g, 0.5f, 0.5f) == std::array<u8, 4>{250, 120, 0, 255});

    auto plain = gfx::Font::from_memory(nullptr, TinyFont().build(), "tiny plain");
    REQUIRE(plain);
    CHECK_FALSE(plain->glyph(0x1F600, 20).color); // just the outline
    CHECK(plain->glyph(0x1F600, 20).visible);
}

TEST_CASE("text: the system's emoji font, where there is one") {
    const char* paths[] = {"/System/Library/Fonts/Apple Color Emoji.ttc", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
                           "/usr/share/fonts/noto/NotoColorEmoji.ttf", "C:/Windows/Fonts/seguiemj.ttf"};
    for (const char* path : paths) {
        auto bytes = read_file_bytes(path);
        if (!bytes) continue;
        auto font = gfx::Font::from_memory(nullptr, std::move(*bytes), path);
        REQUIRE(font);
        const auto& g = font->glyph(0x1F600, 64); // 😀
        CHECK(g.visible);
        CHECK(g.color);
        CHECK(g.source.w > 32);
        MESSAGE("color emoji from ", std::string(path));
    }
}
