#pragma once

// The engine side of the entry point. Games include ph/app/main.hpp instead.

#include "ph/app/engine.hpp"

#include <SDL3/SDL_init.h>

#include <functional>
#include <memory>

namespace ph::detail {
using GameFactory = std::function<std::unique_ptr<Game>()>;
SDL_AppResult app_init(void** state, int argc, char** argv, GameFactory factory, const char* game_asset_dir);
SDL_AppResult app_iterate(void* state);
SDL_AppResult app_event(void* state, SDL_Event* event);
void app_quit(void* state, SDL_AppResult result);
} // namespace ph::detail
