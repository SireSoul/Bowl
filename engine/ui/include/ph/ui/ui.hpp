#pragma once

#include "ph/gfx/text.hpp"
#include "ph/platform/input.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ph::ui {

// Immediate-mode game UI: menus, settings screens, HUD widgets, dialogue. Call widgets every frame between begin and
// end; they return what happened. Works with the mouse, and with keyboard/gamepad focus (up/down to move, confirm to
// press, left/right on sliders) through the input actions ui_up, ui_down, ui_left, ui_right, ui_confirm, ui_back
// (bound to arrows/WASD, Enter/Space, Escape and the gamepad's d-pad, South and East unless the game binds them).
//
//     ui.begin(batch, engine.input(), screen, dt, density);
//     if (ui.begin_panel("pause", {40, 40, 300, 0})) {
//         ui.label("Paused");
//         if (ui.button("Resume")) paused = false;
//         ui.slider("Music", music_volume, 0, 1);
//         ui.end_panel();
//     }
//     ui.end();

struct Style {
    Asset<gfx::Font> font;
    f32 font_size = 18;
    f32 padding = 14;   // inside panels
    f32 spacing = 8;    // between widgets
    f32 widget_height = 34;
    Color text = Color::hex(0xf2f0ea);
    Color text_dim = Color::hex(0xa8a6b8);
    Color panel = Color::hex(0x161a2b, 0.92f);
    Color border = Color::hex(0x3d4670);
    Color widget = Color::hex(0x262c48);
    Color widget_hover = Color::hex(0x323a60);
    Color widget_active = Color::hex(0x495291);
    Color accent = Color::hex(0xffc857);
    // Optional nine-slice art for panels and widgets; insets are the border sizes in texture pixels.
    const gfx::Texture* panel_texture = nullptr;
    Vec4 panel_insets{8, 8, 8, 8};
};

// Reveals text a character at a time, like a dialogue box. Confirm skips to the end, then advances.
struct Typewriter {
    std::string text;
    f32 chars_per_second = 40;
    f32 shown = 0;

    void start(std::string new_text) {
        text = std::move(new_text);
        shown = 0;
    }
    void update(f32 dt) { shown += chars_per_second * dt; }
    void skip() { shown = 1e9f; }
    i32 visible_glyphs() const { return static_cast<i32>(shown); }
    bool done() const;
};

class Context {
public:
    Style style;

    // `pixel_density`: window.pixel_density(), so text is rasterized sharply.
    void begin(gfx::SpriteBatch& batch, Input& input, Vec2 screen_size, f32 dt, f32 pixel_density = 1);
    void end();

    // A panel lays its widgets out top to bottom. rect.h = 0 grows to fit its contents (as of last frame).
    bool begin_panel(std::string_view id, Rect rect);
    void end_panel();

    void label(std::string_view text, Color color = {0, 0, 0, 0}, f32 size = 0);
    bool button(std::string_view text);
    bool checkbox(std::string_view text, bool& value);
    bool slider(std::string_view text, f32& value, f32 min, f32 max);
    void progress(f32 fraction, Color fill = {0, 0, 0, 0});
    void space(f32 height);

    // A dialogue box across the bottom of the screen with a speaker name and typewriter text. Returns true when the
    // player confirms after the text has fully appeared (time to show the next line).
    bool dialogue(std::string_view speaker, Typewriter& text);

    // Whether the UI used the pointer this frame (so the game ignores that click).
    bool wants_pointer() const { return pointer_over_ui_; }
    bool back_pressed() const { return back_; }

private:
    struct Panel {
        u64 id;
        Rect rect;
        f32 cursor_y;
    };
    struct WidgetState {
        bool hovered, focused, pressed, activated;
    };
    WidgetState interact(u64 id, Rect r);
    Rect next_rect(f32 height);
    void frame(Rect r, Color fill, Color border);
    void text(std::string_view s, Vec2 at, Color color, f32 size, gfx::TextAlign align = gfx::TextAlign::Left,
              f32 max_width = 0, i32 max_glyphs = -1);
    u64 make_id(std::string_view label) const;

    gfx::SpriteBatch* batch_ = nullptr;
    Input* input_ = nullptr;
    Vec2 screen_;
    f32 dt_ = 0, density_ = 1;
    std::vector<Panel> panels_;
    std::vector<u64> focus_order_, last_focus_order_;
    std::unordered_map<u64, f32> panel_heights_;
    u64 focused_ = 0, active_ = 0;
    bool confirm_ = false, back_ = false, left_ = false, right_ = false;
    bool pointer_over_ui_ = false, mouse_moved_ = false;
    Vec2 last_mouse_;
    f32 blink_time_ = 0;
};

} // namespace ph::ui
