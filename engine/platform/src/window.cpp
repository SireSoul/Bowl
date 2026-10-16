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

Rect Window::safe_area() const {
    SDL_Rect r{};
    if (!window_ || !SDL_GetWindowSafeArea(window_, &r)) {
        Vec2 s = size();
        return {0, 0, s.x, s.y};
    }
    return {static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.w), static_cast<f32>(r.h)};
}

void Window::set_title(std::string_view title) { SDL_SetWindowTitle(window_, std::string(title).c_str()); }

void Window::set_fullscreen(bool fullscreen) { SDL_SetWindowFullscreen(window_, fullscreen); }

bool Window::set_icon(u32 width, u32 height, std::span<const u8> rgba) {
    if (!window_ || rgba.size() < static_cast<usize>(width) * height * 4) return false;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<u8*>(rgba.data()), static_cast<int>(width * 4));
    if (!surface) return false;
    bool ok = SDL_SetWindowIcon(window_, surface);
    SDL_DestroySurface(surface);
    return ok;
}

bool Window::fullscreen() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }

} // namespace ph
