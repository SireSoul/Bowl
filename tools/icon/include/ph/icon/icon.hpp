#pragma once

#include "ph/core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

// Application icons from one picture: macOS .icns, Windows .ico, PNGs at any size, and a C++ file that puts the icon
// into the executable for the window (ph_add_game's ICON). Pixels in memory, so it's testable; ph-icon is the
// command-line tool around it.
namespace ph::icon {

struct Image {
    u32 width = 0, height = 0;
    std::vector<u8> rgba; // width * height * 4
};

std::optional<Image> decode_png(std::span<const u8> bytes);
std::vector<u8> encode_png(const Image& image);

// A square `size` x `size` copy. A picture that isn't square is centered on a clear background. `pixel_art` keeps hard
// pixel edges (nearest pixel) instead of smoothing.
Image resize(const Image& image, u32 size, bool pixel_art = false);

// Every size macOS asks for, 16 to 1024 (PNG inside).
std::vector<u8> make_icns(const Image& image, bool pixel_art = false);
// 16 to 256 (PNG inside; Windows Vista and later).
std::vector<u8> make_ico(const Image& image, bool pixel_art = false);
// C++ source that registers `png` as the game's icon (ph::embedded_icon()).
std::string make_cpp(std::span<const u8> png);

} // namespace ph::icon
