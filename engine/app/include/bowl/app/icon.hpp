#pragma once

#include "bowl/core/types.hpp"

#include <span>

namespace bowl {

// The game's icon as a PNG, put into the executable by bowl_add_game(... ICON icon.png); empty without one. The App
// shows it on the window (title bar, taskbar, and the Dock outside an .app bundle, which has its own .icns).
std::span<const u8> embedded_icon();

// What the file bowl_add_game makes uses to register the icon.
struct EmbeddedIcon {
    EmbeddedIcon(const u8* png, usize size);
};

} // namespace bowl
