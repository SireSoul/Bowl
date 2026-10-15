#pragma once

#include "ph/core/math.hpp"
#include "ph/core/types.hpp"

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

    void set_title(std::string_view title);
    void set_fullscreen(bool fullscreen);
    bool fullscreen() const;

private:
    SDL_Window* window_ = nullptr;
};

} // namespace ph
