#include "ph/core/file.hpp"
#include "ph/core/utf8.hpp"
#include "ph/gfx/text.hpp"

#include <doctest/doctest.h>

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
