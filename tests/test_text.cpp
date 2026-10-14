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
