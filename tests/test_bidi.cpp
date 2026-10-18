#include "bowl/core/file.hpp"
#include "bowl/core/utf8.hpp"
#include "bowl/gfx/bidi.hpp"
#include "bowl/gfx/text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

using namespace bowl;
using namespace bowl::gfx;

namespace {

std::vector<u32> decode(std::string_view s) {
    std::vector<u32> out;
    for (usize i = 0; i < s.size();) out.push_back(utf8::next(s, i));
    return out;
}

std::unique_ptr<Font> load_font(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return nullptr;
    return Font::from_memory(nullptr, std::move(*bytes), path.string());
}

// The characters of laid-out text, left to right on screen.
std::vector<u32> on_screen(const std::vector<PlacedGlyph>& glyphs) {
    std::vector<PlacedGlyph> sorted = glyphs;
    std::stable_sort(sorted.begin(), sorted.end(), [](const PlacedGlyph& a, const PlacedGlyph& b) { return a.position.x < b.position.x; });
    std::vector<u32> out;
    for (const PlacedGlyph& g : sorted) out.push_back(g.codepoint);
    return out;
}

constexpr u32 alef = 0x05D0, bet = 0x05D1, gimel = 0x05D2; // Hebrew

} // namespace

TEST_CASE("bidi: character types") {
    using bidi::Type;
    CHECK(bidi::type_of('a') == Type::L);
    CHECK(bidi::type_of(0x4E2D) == Type::L);  // 中
    CHECK(bidi::type_of(alef) == Type::R);
    CHECK(bidi::type_of(0x0628) == Type::AL); // Arabic beh
    CHECK(bidi::type_of('7') == Type::EN);
    CHECK(bidi::type_of(0x0663) == Type::AN); // Arabic-Indic three
    CHECK(bidi::type_of(0x064E) == Type::NSM); // fatha
    CHECK(bidi::type_of(' ') == Type::WS);
    CHECK(bidi::type_of('\n') == Type::B);
    CHECK(bidi::type_of('!') == Type::ON);
    CHECK(bidi::type_of('$') == Type::ET);
    CHECK(bidi::type_of(',') == Type::CS);
    CHECK(bidi::type_of(0x1F600) == Type::ON);
    CHECK(bidi::mirror('(') == ')');
    CHECK(bidi::mirror(']') == '[');
    CHECK(bidi::mirror('a') == 'a');
}

TEST_CASE("bidi: levels and visual order") {
    auto ltr = decode("abc \xD7\x90\xD7\x91\xD7\x92 def"); // abc אבג def
    bidi::Paragraph p = bidi::resolve(ltr);
    CHECK(p.level == 0);
    CHECK(p.levels == std::vector<u8>{0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0});
    CHECK(bidi::visual_order(ltr, p.levels, p.level) == std::vector<u32>{0, 1, 2, 3, 6, 5, 4, 7, 8, 9, 10});

    auto rtl = decode("\xD7\x90\xD7\x91 cd"); // אב cd
    p = bidi::resolve(rtl);
    CHECK(p.level == 1);
    CHECK(p.levels == std::vector<u8>{1, 1, 1, 2, 2});
    CHECK(bidi::visual_order(rtl, p.levels, p.level) == std::vector<u32>{3, 4, 2, 1, 0});

    // Numbers in right-to-left text read left to right, as a unit with their separators.
    auto numbers = decode("\xD7\x90 12.5 \xD7\x91"); // א 12.5 ב
    p = bidi::resolve(numbers);
    CHECK(p.levels == std::vector<u8>{1, 1, 2, 2, 2, 2, 1, 1});
    CHECK(bidi::visual_order(numbers, p.levels, p.level) == std::vector<u32>{7, 6, 2, 3, 4, 5, 1, 0});

    // Forced direction, and whitespace at the end of a line staying at the end.
    p = bidi::resolve(decode("abc"), bidi::Direction::RightToLeft);
    CHECK(p.level == 1);
    CHECK(p.levels == std::vector<u8>{2, 2, 2});
    auto trailing = decode("ab \xD7\x90\xD7\x91 ");
    p = bidi::resolve(trailing);
    CHECK(bidi::visual_order(trailing, p.levels, p.level) == std::vector<u32>{0, 1, 2, 4, 3, 5});
}

TEST_CASE("text: right-to-left characters are drawn in reading order") {
    auto font = load_font(std::filesystem::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "fonts" / "Roboto-Medium.ttf");
    REQUIRE(font);
    TextStyle style;
    style.size = 20;
    // Roboto has no Hebrew, so those are drawn as '?', but in the right places, and PlacedGlyph says which is which.
    auto glyphs = layout_text(*font, "abc \xD7\x90\xD7\x91\xD7\x92", {}, style);
    CHECK(on_screen(glyphs) == std::vector<u32>{'a', 'b', 'c', gimel, bet, alef});
    // Reading order for typewriter reveals: alef comes fourth, though it's drawn last.
    auto alef_glyph = std::find_if(glyphs.begin(), glyphs.end(), [](const PlacedGlyph& g) { return g.codepoint == alef; });
    REQUIRE(alef_glyph != glyphs.end());
    CHECK(alef_glyph->index == 3);
    style.max_glyphs = 4;
    CHECK(on_screen(layout_text(*font, "abc \xD7\x90\xD7\x91\xD7\x92", {}, style)) == std::vector<u32>{'a', 'b', 'c', alef});
    style.max_glyphs = -1;

    // A right-to-left paragraph: brackets are mirrored, and right-aligned text ends at the position.
    style.align = TextAlign::Right;
    auto rtl = layout_text(*font, "\xD7\x90(\xD7\x91)", {100, 0}, style); // א(ב)
    CHECK(on_screen(rtl) == std::vector<u32>{'(', bet, ')', alef});
    f32 right = 0;
    for (const PlacedGlyph& g : rtl) right = std::max(right, g.position.x + g.size.x);
    CHECK(right <= 100.5f);

    // The direction can be forced: a left-to-right paragraph keeps "(" first.
    style.align = TextAlign::Left;
    style.direction = bidi::Direction::LeftToRight;
    CHECK(on_screen(layout_text(*font, "(a)", {}, style)) == std::vector<u32>{'(', 'a', ')'});
}

TEST_CASE("text: shaping joins Arabic letters (HarfBuzz builds)") {
    if (!text_shaping()) return;
    std::unique_ptr<Font> font;
    for (const char* path : {"/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf", "C:/Windows/Fonts/arial.ttf"})
        if ((font = load_font(path))) break;
    if (!font) return;
    TextStyle style;
    style.size = 32;
    // Beh three times: joined forms replace the letter on its own (fonts differ in which forms they draw apart), and
    // the joined word is narrower than three separate letters.
    auto joined = layout_text(*font, "\xD8\xA8\xD8\xA8\xD8\xA8", {}, style);
    auto alone = layout_text(*font, "\xD8\xA8", {}, style);
    REQUIRE(joined.size() >= 3);
    REQUIRE(alone.size() == 1);
    CHECK(std::any_of(joined.begin(), joined.end(), [&](const PlacedGlyph& g) { return g.glyph_index != alone[0].glyph_index; }));
    CHECK(measure_text(*font, "\xD8\xA8\xD8\xA8\xD8\xA8", style).x < 3 * measure_text(*font, "\xD8\xA8", style).x - 1);
    // Drawn right to left: the first letter (the initial form) is on the right.
    auto first = std::find_if(joined.begin(), joined.end(), [](const PlacedGlyph& g) { return g.index == 0; });
    for (const PlacedGlyph& g : joined) CHECK(g.position.x <= first->position.x);

    // Devanagari: "namaste" is six characters; स्त is one conjunct, so it draws as fewer glyphs.
    for (const char* path : {"/System/Library/Fonts/Kohinoor.ttc", "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf"})
        if (auto devanagari = load_font(path)) {
            auto namaste = layout_text(*devanagari, "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", {}, style);
            CHECK(namaste.size() < 6);
            CHECK(namaste.size() >= 3);
            break;
        }
}

TEST_CASE("text: right-to-left text ending in a newline") {
    // The empty line after the newline read one past the end of the text's levels.
    auto font = load_font(std::filesystem::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "fonts" / "Roboto-Medium.ttf");
    REQUIRE(font);
    TextStyle style;
    style.size = 20;
    CHECK(layout_text(*font, "\xD7\x90\xD7\x91\n", {}, style).size() == 2);
    CHECK(layout_text(*font, "a\n\xD7\x90\n\n", {}, style).size() == 2);
}
