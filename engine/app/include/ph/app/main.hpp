#pragma once

// Include in exactly one .cpp of your game and write PH_MAIN(YourGame).
//
// Uses SDL3's callback entry points instead of a main() loop, which is what the web (Emscripten), iOS and Android
// need — the same game code runs everywhere.

#include "ph/app/entry.hpp"

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#ifdef PH_GAME_ASSET_SOURCE_DIR
#define PH_DETAIL_GAME_ASSETS PH_GAME_ASSET_SOURCE_DIR
#else
#define PH_DETAIL_GAME_ASSETS ""
#endif

#define PH_MAIN(GameType)                                                                                             \
    SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {                                                  \
        return ::ph::detail::app_init(state, argc, argv, [] { return std::unique_ptr<::ph::Game>(new GameType()); },  \
                                      PH_DETAIL_GAME_ASSETS);                                                         \
    }                                                                                                                 \
    SDL_AppResult SDL_AppIterate(void* state) { return ::ph::detail::app_iterate(state); }                           \
    SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) { return ::ph::detail::app_event(state, event); }      \
    void SDL_AppQuit(void* state, SDL_AppResult result) { ::ph::detail::app_quit(state, result); }
