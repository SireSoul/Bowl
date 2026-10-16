#pragma once

// Include in exactly one .cpp of your game and write BOWL_MAIN(YourGame).
//
// Uses SDL3's callback entry points instead of a main() loop. The web (Emscripten), iOS and Android need those, and
// using them everywhere keeps the game code identical on every platform. In a hot-reload build
// (bowl_add_game(... HOT_RELOAD)) the game is a library instead, and BOWL_MAIN exports the function the host uses to
// create the game.

#include "bowl/app/entry.hpp"

#if defined(BOWL_HOT_RELOAD_LIBRARY)

#if defined(_WIN32)
#define BOWL_DETAIL_EXPORT extern "C" __declspec(dllexport)
#else
#define BOWL_DETAIL_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#define BOWL_MAIN(GameType)                                                                                            \
    BOWL_DETAIL_EXPORT ::bowl::Game* bowl_create_game() { return new GameType(); }

#else

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#ifdef BOWL_GAME_ASSET_SOURCE_DIR
#define BOWL_DETAIL_GAME_ASSETS BOWL_GAME_ASSET_SOURCE_DIR
#else
#define BOWL_DETAIL_GAME_ASSETS ""
#endif

#define BOWL_MAIN(GameType)                                                                                            \
    SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {                                                   \
        return ::bowl::detail::app_init(                                                                               \
            state, argc, argv, [] { return std::unique_ptr<::bowl::Game>(new GameType()); },                           \
            BOWL_DETAIL_GAME_ASSETS);                                                                                  \
    }                                                                                                                  \
    SDL_AppResult SDL_AppIterate(void* state) { return ::bowl::detail::app_iterate(state); }                           \
    SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) { return ::bowl::detail::app_event(state, event); }      \
    void SDL_AppQuit(void* state, SDL_AppResult result) { ::bowl::detail::app_quit(state, result); }

#endif
