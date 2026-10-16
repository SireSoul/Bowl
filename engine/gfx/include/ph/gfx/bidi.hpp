#pragma once

#include "ph/core/types.hpp"

#include <span>
#include <vector>

// Text that mixes left-to-right and right-to-left scripts (Hebrew, Arabic): which way each character runs, and the
// order to draw a line's characters in. draw_text uses it by itself; it's here for code that lays out text its own
// way (a text field's cursor, say).
//
// This is the core of the Unicode bidirectional algorithm (UAX #9): paragraph direction, weak and neutral types,
// implicit levels, reordering and mirrored brackets. Explicit embedding and isolate controls (U+202A..U+202E,
// U+2066..U+2069) are ignored, and character types come from a compact table rather than the full Unicode data, which
// is right for the common scripts.
namespace ph::gfx::bidi {

enum class Type : u8 { L, R, AL, EN, AN, ES, ET, CS, NSM, B, S, WS, ON };
Type type_of(u32 codepoint);

enum class Direction : u8 { Auto, LeftToRight, RightToLeft };

// Embedding levels for one paragraph's characters: even runs left to right, odd right to left. Auto takes the
// paragraph's direction from its first letter.
struct Paragraph {
    u8 level = 0; // 0: left to right, 1: right to left
    std::vector<u8> levels;
};
Paragraph resolve(std::span<const u32> codepoints, Direction direction = Direction::Auto);

// The order to draw a line's characters in, as indexes into `levels` (a line's slice of a paragraph). Whitespace at
// the end of the line goes to the paragraph's level first, so it stays at the end.
std::vector<u32> visual_order(std::span<const u32> codepoints, std::span<const u8> levels, u8 paragraph_level);

// The mirrored form of a bracket-like character, drawn in right-to-left runs: '(' for ')', and so on. Others are
// returned as they are.
u32 mirror(u32 codepoint);

} // namespace ph::gfx::bidi
