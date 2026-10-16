#include "bowl/platform/system.hpp"

#include <SDL3/SDL.h>

#include <fstream>
#include <string>
#include <string_view>

namespace bowl {

namespace {

std::optional<bool> override_deck;

bool detect_steam_deck() {
    const char* steam = SDL_getenv("SteamDeck");
    if (steam && std::string_view(steam) == "1") return true;
#if defined(__linux__)
    auto read = [](const char* path) {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        return line;
    };
    if (read("/sys/devices/virtual/dmi/id/board_vendor") == "Valve") {
        std::string board = read("/sys/devices/virtual/dmi/id/board_name");
        return board == "Jupiter" || board == "Galileo"; // LCD, OLED
    }
#endif
    return false;
}

} // namespace

bool is_steam_deck() {
    if (override_deck) return *override_deck;
    static const bool detected = detect_steam_deck();
    return detected;
}

void set_steam_deck(std::optional<bool> value) { override_deck = value; }

} // namespace bowl
