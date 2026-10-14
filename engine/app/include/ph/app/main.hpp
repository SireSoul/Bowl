#pragma once

// Include in exactly one .cpp of your game and write PH_MAIN(YourGame).
//
// Uses SDL3's callback entry points instead of a main() loop, which is what the web (Emscripten), iOS and Android
// need — the same game code runs everywhere. In a hot-reload build (ph_add_game(... HOT_RELOAD)) the game is a
// library instead, and PH_MAIN exports the function the host uses to create the game.

#include "ph/app/entry.hpp"

#if defined(PH_HOT_RELOAD_LIBRARY)

#if defined(_WIN32)
#define PH_DETAIL_EXPORT extern "C" __declspec(dllexport)
#else
#define PH_DETAIL_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#define PH_MAIN(GameType)                                                                                             \
    PH_DETAIL_EXPORT ::ph::Game* ph_create_game() { return new GameType(); }

#else

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

#endif
