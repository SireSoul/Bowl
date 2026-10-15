#pragma once

#include "ph/core/types.hpp"

#include <span>

namespace ph {

// The game's icon as a PNG, put into the executable by ph_add_game(... ICON icon.png); empty without one. The App
// shows it on the window (title bar, taskbar, and the Dock outside an .app bundle, which has its own .icns).
std::span<const u8> embedded_icon();

// What the file ph_add_game makes uses to register the icon.
struct EmbeddedIcon {
    EmbeddedIcon(const u8* png, usize size);
};

} // namespace ph
