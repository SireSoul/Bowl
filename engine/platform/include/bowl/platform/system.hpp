#pragma once

#include <optional>

namespace bowl {

// Whether the game runs on a Steam Deck: Steam says so (the SteamDeck environment variable), or the hardware is one.
// On the Deck the App starts fullscreen (AppConfig::fullscreen_on_steam_deck). `--deck` pretends to be one in a
// 1280 x 800 window, to check how the game looks and reads there.
bool is_steam_deck();
// Overrides the answer (`--deck` sets it); nothing goes back to asking the system.
void set_steam_deck(std::optional<bool> value);

} // namespace bowl
