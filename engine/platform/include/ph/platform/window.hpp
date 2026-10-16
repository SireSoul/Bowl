#pragma once

#include "ph/core/math.hpp"
#include "ph/core/types.hpp"

#include <span>
#include <string>
#include <string_view>

struct SDL_Window;

namespace ph {

struct WindowDesc {
    std::string title = "Bowl";
    i32 width = 1280;
    i32 height = 720;
    bool resizable = true;
    bool fullscreen = false;
};

class Window : NonCopyable {
public:
    Window() = default;
    ~Window();

    bool create(const WindowDesc& desc);
    void destroy();

    SDL_Window* handle() const { return window_; }
    // Size in logical points (what game coordinates use) and in physical pixels (high-DPI displays).
    Vec2 size() const;
    Vec2 pixel_size() const;
    f32 pixel_density() const;
    // The part of the window nothing covers, in logical points: on phones, inside the notch, the rounded corners and
    // the home indicator. Keep buttons and text in it. The whole window elsewhere.
    Rect safe_area() const;

    void set_title(std::string_view title);
    void set_fullscreen(bool fullscreen);
    bool fullscreen() const;
    // The title bar and taskbar icon, from RGBA8 pixels. The App sets the game's ICON (ph_add_game) by itself.
    bool set_icon(u32 width, u32 height, std::span<const u8> rgba);

private:
    SDL_Window* window_ = nullptr;
};

} // namespace ph
