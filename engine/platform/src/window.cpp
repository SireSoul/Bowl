#include "ph/platform/window.hpp"

#include "ph/core/log.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace ph {

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (!window_) {
        error("window: SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    Vec2 px = pixel_size();
    info("window: {}x{} points, {}x{} pixels", desc.width, desc.height, px.x, px.y);
    return true;
}

void Window::destroy() {
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
}

Vec2 Window::size() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    return {static_cast<f32>(w), static_cast<f32>(h)};
}

Vec2 Window::pixel_size() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return {static_cast<f32>(w), static_cast<f32>(h)};
}

f32 Window::pixel_density() const { return SDL_GetWindowPixelDensity(window_); }

void Window::set_title(std::string_view title) { SDL_SetWindowTitle(window_, std::string(title).c_str()); }

void Window::set_fullscreen(bool fullscreen) { SDL_SetWindowFullscreen(window_, fullscreen); }

bool Window::fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

} // namespace ph
