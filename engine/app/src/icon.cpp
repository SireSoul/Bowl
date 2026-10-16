#include "ph/app/icon.hpp"

namespace ph {

namespace {
// Plain values, so they're set (to nothing) before any constructor runs, including the icon file's.
const u8* icon_png = nullptr;
usize icon_size = 0;
} // namespace

EmbeddedIcon::EmbeddedIcon(const u8* png, usize size) {
    icon_png = png;
    icon_size = size;
}

std::span<const u8> embedded_icon() { return {icon_png, icon_size}; }

} // namespace ph
