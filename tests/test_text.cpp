#include "bowl/core/file.hpp"
#include "bowl/core/utf8.hpp"
#include "bowl/gfx/text.hpp"
#include "tiny_font.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>

using namespace bowl;
using namespace bowl::testing;

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
    auto bytes = read_file_bytes(std::filesystem::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "fonts" / "Roboto-Medium.ttf");
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
    add_colr(tiny);
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
    add_cbdt(tiny, png_of(20, 0, 200, 0));
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
    add_sbix(tiny, png_of(20, 250, 120, 0));
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

TEST_CASE("text: fonts with only pictures load, like Noto Color Emoji") {
    // Noto Color Emoji has CBDT pictures and no outlines at all: no glyf or loca table.
    TinyFont tiny;
    std::erase_if(tiny.tables, [](const auto& t) { return t.first == "glyf" || t.first == "loca"; });
    add_cbdt(tiny, png_of(20, 0, 200, 0));
    auto font = gfx::Font::from_memory(nullptr, tiny.build(), "pictures only");
    REQUIRE(font);
    CHECK(font->has_glyph(0x1F600));
    const auto& g = font->glyph(0x1F600, 20);
    REQUIRE(g.visible);
    CHECK(g.color);
    CHECK(g.source.w == 20);
    CHECK(pixel(*font, g, 0.5f, 0.5f) == std::array<u8, 4>{0, 200, 0, 255});
    CHECK(g.advance == doctest::Approx(20.0f)); // hmtx: 1000 units at 20 px per em

    const auto& a = font->glyph('A', 20); // in the character map, but there's no picture or outline to draw
    CHECK_FALSE(a.visible);
    CHECK(a.advance == doctest::Approx(20.0f));
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
