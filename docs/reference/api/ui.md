# ui

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/ui/ui.hpp](#bowluiuihpp)

## bowl/ui/ui.hpp

`#include "bowl/ui/ui.hpp"` · namespace `bowl::ui`

Immediate-mode game UI: menus, settings screens, HUD widgets, dialogue. Call widgets every frame between begin and
end; they return what happened. Works with the mouse, and with keyboard/gamepad focus (up/down to move, confirm to
press, left/right on sliders) through the input actions ui_up, ui_down, ui_left, ui_right, ui_confirm, ui_back
(bound to arrows/WASD, Enter/Space, Escape and the gamepad's d-pad, South and East unless the game binds them).

Widgets draw into a batch you begin yourself with a screen camera, so your own HUD can share it:

```cpp
batch.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None);
ui.begin(batch, engine.input(), screen, dt, density);
if (ui.begin_panel("pause", {40, 40, 300, 0})) {
    ui.label("Paused");
    if (ui.button("Resume")) paused = false;
    ui.slider("Music", music_volume, 0, 1);
    ui.end_panel();
}
ui.end();
engine.renderer().submit_ui(batch);
```

### Style

```cpp
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
```

### Typewriter

Reveals text a character at a time, like a dialogue box. Confirm skips to the end, then advances.

```cpp
struct Typewriter
```

```cpp
std::string text;
f32 chars_per_second = 40;
f32 shown = 0;
```

```cpp
void start(std::string new_text);
void update(f32 dt);
void skip();
i32 visible_glyphs() const;
bool done() const;
```

### Context

```cpp
class Context
```

```cpp
Style style;
```

`pixel_density`: window.pixel_density(), so text is rasterized sharply.

```cpp
void begin(gfx::SpriteBatch& batch, Input& input, Vec2 screen_size, f32 dt, f32 pixel_density = 1);
void end();
```

A panel lays its widgets out top to bottom. rect.h = 0 grows to fit its contents (as of last frame).

```cpp
bool begin_panel(std::string_view id, Rect rect);
void end_panel();
```

```cpp
void label(std::string_view text, Color color = {0, 0, 0, 0}, f32 size = 0);
bool button(std::string_view text);
bool checkbox(std::string_view text, bool& value);
bool slider(std::string_view text, f32& value, f32 min, f32 max);
void progress(f32 fraction, Color fill = {0, 0, 0, 0});
void space(f32 height);
```

A dialogue box across the bottom of the screen with a speaker name and typewriter text. Returns true when the
player confirms after the text has fully appeared (time to show the next line).

```cpp
bool dialogue(std::string_view speaker, Typewriter& text);
```

Whether the UI used the pointer this frame (so the game ignores that click).

```cpp
bool wants_pointer() const;
bool back_pressed() const;
```

