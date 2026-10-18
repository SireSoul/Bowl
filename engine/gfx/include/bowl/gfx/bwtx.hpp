#pragma once

#include "bowl/core/types.hpp"

#include <span>
#include <vector>

namespace bowl::gfx {

// BWTX: the picture format bowl-cook turns PNG and JPEG files into. RGBA8 pixels, losslessly packed so they decode
// several times faster than a PNG while staying about its size. The texture loader (and decode_png) tells it apart by
// its first four bytes, "BWTX", so a cooked picture keeps its .png name.
//
// The layout: "BWTX", a version byte (1), the channel count (4), two zero bytes, the width and height as little-endian
// u32s, then the pixels as a QOI stream (the "Quite OK Image" format, qoiformat.org) ending in seven 0 bytes and a 1.

inline constexpr u32 bwtx_max_side = 16384; // as for every texture

bool is_bwtx(std::span<const u8> bytes);
// `rgba` holds width * height * 4 bytes, row by row from the top.
std::vector<u8> encode_bwtx(u32 width, u32 height, std::span<const u8> rgba);
// False for anything that isn't a whole BWTX picture of at most bwtx_max_side a side.
bool decode_bwtx(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba);

} // namespace bowl::gfx
