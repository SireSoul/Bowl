# platform

Made by `ph-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target ph_api_docs`.

- [ph/platform/input.hpp](#phplatforminputhpp)
- [ph/platform/system.hpp](#phplatformsystemhpp)
- [ph/platform/window.hpp](#phplatformwindowhpp)

## ph/platform/input.hpp

`#include "ph/platform/input.hpp"` · namespace `ph`

```cpp
union SDL_Event;
```

Values are SDL scancodes (physical key positions: WASD stays WASD on AZERTY keyboards).

```cpp
enum class Key : u16 {
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,
    Minus = 45, Equals = 46, LeftBracket = 47, RightBracket = 48, Backslash = 49,
    Semicolon = 51, Apostrophe = 52, Grave = 53, Comma = 54, Period = 55, Slash = 56,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Insert = 73, Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78,
    Right = 79, Left = 80, Down = 81, Up = 82,
    LeftCtrl = 224, LeftShift = 225, LeftAlt = 226, LeftSuper = 227,
    RightCtrl = 228, RightShift = 229, RightAlt = 230, RightSuper = 231,
};
```

```cpp
enum class MouseButton : u8 { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };
```

Positional names (South = A on Xbox, Cross on PlayStation, B on Nintendo). Values match SDL_GamepadButton.

```cpp
enum class GamepadButton : u8 {
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
};
enum class GamepadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };
```

### InputBinding

One physical input an action listens to.

```cpp
struct InputBinding
```

```cpp
enum class Kind : u8 { Key, Mouse, GamepadButton, GamepadAxis };
Kind kind = Kind::Key;
u8 code = 0;
i8 gamepad = -1;      // -1 = any gamepad
f32 direction = 1.0f; // axes: +1 or -1, which half of the axis counts
f32 threshold = 0.5f; // axes: how far counts as "down"
```

```cpp
static InputBinding key(Key k);
static InputBinding mouse(MouseButton b);
static InputBinding button(GamepadButton b, i8 pad = -1);
static InputBinding axis(GamepadAxis a, f32 direction, i8 pad = -1, f32 threshold = 0.5f);
```

### Input

Keyboard, mouse and gamepads, plus named actions bound to any of them.

```cpp
input.bind("jump", {InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
if (input.pressed("jump")) ...
Vec2 move = input.vector("left", "right", "up", "down");
```
"pressed"/"released" are edges. Inside fixed-timestep updates they mean "since the previous fixed tick", in
per-frame code "since the previous frame". A tap is never lost or seen twice, whatever the frame rate.

```cpp
class Input : NonCopyable
```

```cpp
static constexpr int max_gamepads = 4;
enum class Context : u8 { Frame = 0, Fixed = 1 };
```

```cpp
Input();
~Input();
```

**Raw devices**

```cpp
bool down(Key k) const;
bool pressed(Key k) const;
bool released(Key k) const;
```

```cpp
bool down(MouseButton b) const;
bool pressed(MouseButton b) const;
bool released(MouseButton b) const;
Vec2 mouse_position() const; // window coordinates, logical points
Vec2 mouse_delta() const; // this frame
f32 wheel() const; // this frame
```

```cpp
bool gamepad_connected(int pad) const;
bool down(GamepadButton b, int pad = 0) const;
bool pressed(GamepadButton b, int pad = 0) const;
f32 axis(GamepadAxis a, int pad = 0) const;
void rumble(int pad, f32 low, f32 high, u32 milliseconds);
```

What the player used last: show gamepad buttons in prompts while it's a gamepad, keys while it's the keyboard.

```cpp
enum class Device : u8 { KeyboardMouse, Gamepad };
Device last_device() const;
```

Text typed this frame (UTF-8), for name entry and chat.

```cpp
std::string_view text() const;
```

**Actions**

Adds bindings to an action (creating it if needed); an action can have any number.

```cpp
void bind(std::string_view action, InputBinding binding);
void bind(std::string_view action, std::initializer_list<InputBinding> bindings);
void unbind(std::string_view action);
void unbind_all();
bool has_action(std::string_view action) const;
const std::vector<InputBinding>* bindings(std::string_view action) const;
```

Rebinding: the first key/button/stick pushed this frame, e.g. for "press a key for Jump" screens.

```cpp
std::optional<InputBinding> last_pressed() const;
```

Readable name for prompts and settings screens: "Space", "Left mouse", "Gamepad a", "Gamepad leftx+".

```cpp
static std::string describe(const InputBinding& binding);
```

Save/restore bindings (readable JSON: {"jump": [{"key": "Space"}, {"button": "a"}]}). Loading replaces the
bindings of the actions it mentions and leaves others alone.

```cpp
json::Value save_bindings() const;
void load_bindings(const json::Value& json);
bool down(std::string_view action) const;
bool pressed(std::string_view action) const;
bool released(std::string_view action) const;
```

0..1: analog for axes and triggers, 0 or 1 for buttons.

```cpp
f32 value(std::string_view action) const;
```

Two-axis input from four actions, length clamped to 1, with a radial dead zone.

```cpp
Vec2 vector(std::string_view neg_x, std::string_view pos_x, std::string_view neg_y, std::string_view pos_y,
            f32 dead_zone = 0.15f) const;
```

Called by the App each frame: begin_frame, handle_event for each event, end_events, then
begin_fixed_tick/end_fixed_tick around every fixed tick.

```cpp
void begin_frame();
void end_events();
void begin_fixed_tick();
void end_fixed_tick();
void set_context(Context c);
void handle_event(const SDL_Event& event);
```

Dev UI captured the keyboard/mouse: games see nothing new until released.

```cpp
void set_blocked(bool keyboard, bool mouse);
```

## ph/platform/system.hpp

`#include "ph/platform/system.hpp"` · namespace `ph`

Whether the game runs on a Steam Deck: Steam says so (the SteamDeck environment variable), or the hardware is one.
On the Deck the App starts fullscreen (AppConfig::fullscreen_on_steam_deck). `--deck` pretends to be one in a
1280 x 800 window, to check how the game looks and reads there.

```cpp
bool is_steam_deck();
```

Overrides the answer (`--deck` sets it); nothing goes back to asking the system.

```cpp
void set_steam_deck(std::optional<bool> value);
```

## ph/platform/window.hpp

`#include "ph/platform/window.hpp"` · namespace `ph`

### WindowDesc

```cpp
struct WindowDesc {
    std::string title = "Bowl";
    i32 width = 1280;
    i32 height = 720;
    bool resizable = true;
    bool fullscreen = false;
};
```

### Window

```cpp
class Window : NonCopyable
```

```cpp
Window() = default;
~Window();
```

```cpp
bool create(const WindowDesc& desc);
void destroy();
```

```cpp
SDL_Window* handle() const;
```

Size in logical points (what game coordinates use) and in physical pixels (high-DPI displays).

```cpp
Vec2 size() const;
Vec2 pixel_size() const;
f32 pixel_density() const;
```

```cpp
void set_title(std::string_view title);
void set_fullscreen(bool fullscreen);
bool fullscreen() const;
```

The title bar and taskbar icon, from RGBA8 pixels. The App sets the game's ICON (ph_add_game) by itself.

```cpp
bool set_icon(u32 width, u32 height, std::span<const u8> rgba);
```

