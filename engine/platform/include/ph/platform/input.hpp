#pragma once

#include "ph/core/json.hpp"
#include "ph/core/math.hpp"
#include "ph/core/types.hpp"

#include <array>
#include <bitset>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace ph {

// Values are SDL scancodes (physical key positions: WASD stays WASD on AZERTY keyboards).
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

enum class MouseButton : u8 { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// Positional names (South = A on Xbox, Cross on PlayStation, B on Nintendo). Values match SDL_GamepadButton.
enum class GamepadButton : u8 {
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
};
enum class GamepadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };

// One physical input an action listens to.
struct InputBinding {
    enum class Kind : u8 { Key, Mouse, GamepadButton, GamepadAxis };
    Kind kind = Kind::Key;
    u8 code = 0;
    i8 gamepad = -1;      // -1 = any gamepad
    f32 direction = 1.0f; // axes: +1 or -1, which half of the axis counts
    f32 threshold = 0.5f; // axes: how far counts as "down"

    static InputBinding key(Key k) { return {Kind::Key, static_cast<u8>(static_cast<u16>(k) & 0xFF), -1, 1, 0.5f}; }
    static InputBinding mouse(MouseButton b) { return {Kind::Mouse, static_cast<u8>(b), -1, 1, 0.5f}; }
    static InputBinding button(GamepadButton b, i8 pad = -1) { return {Kind::GamepadButton, static_cast<u8>(b), pad, 1, 0.5f}; }
    static InputBinding axis(GamepadAxis a, f32 direction, i8 pad = -1, f32 threshold = 0.5f) {
        return {Kind::GamepadAxis, static_cast<u8>(a), pad, direction, threshold};
    }
};

// Keyboard, mouse and gamepads, plus named actions bound to any of them.
//
//     input.bind("jump", {InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
//     if (input.pressed("jump")) ...
//     Vec2 move = input.vector("left", "right", "up", "down");
//
// "pressed"/"released" are edges. Inside fixed-timestep updates they mean "since the previous fixed tick", in
// per-frame code "since the previous frame". A tap is never lost or seen twice, whatever the frame rate.
class Input : NonCopyable {
public:
    static constexpr int max_gamepads = 4;
    enum class Context : u8 { Frame = 0, Fixed = 1 };

    Input();
    ~Input();

    // Raw devices

    bool down(Key k) const { return down_.test(key_code(k)); }
    bool pressed(Key k) const { return pressed_[ctx()].test(key_code(k)); }
    bool released(Key k) const { return released_[ctx()].test(key_code(k)); }

    bool down(MouseButton b) const { return down_.test(mouse_code(b)); }
    bool pressed(MouseButton b) const { return pressed_[ctx()].test(mouse_code(b)); }
    bool released(MouseButton b) const { return released_[ctx()].test(mouse_code(b)); }
    Vec2 mouse_position() const { return mouse_; }   // window coordinates, logical points
    Vec2 mouse_delta() const { return mouse_delta_; } // this frame
    f32 wheel() const { return wheel_; }             // this frame

    bool gamepad_connected(int pad) const { return pad >= 0 && pad < max_gamepads && pads_[pad] != nullptr; }
    bool down(GamepadButton b, int pad = 0) const { return down_.test(pad_code(pad, b)); }
    bool pressed(GamepadButton b, int pad = 0) const { return pressed_[ctx()].test(pad_code(pad, b)); }
    f32 axis(GamepadAxis a, int pad = 0) const;
    void rumble(int pad, f32 low, f32 high, u32 milliseconds);

    // Fingers on a touch screen, in window coordinates (logical points). A touch is listed from the frame it goes down
    // (pressed) to the frame it lifts (released). Taps also click: the first finger moves the mouse and presses
    // MouseButton::Left, so buttons and the UI work by touch as they are.
    struct Touch {
        u64 id = 0;
        Vec2 position, start; // now, and where it went down
        bool pressed = false, released = false;
    };
    std::span<const Touch> touches() const { return touches_; }

    // What the player used last: show gamepad buttons in prompts while it's a gamepad, keys while it's the keyboard.
    enum class Device : u8 { KeyboardMouse, Gamepad, Touch };
    Device last_device() const { return last_device_; }

    // Text typed this frame (UTF-8), for name entry and chat.
    std::string_view text() const { return text_; }

    // Actions

    // Adds bindings to an action (creating it if needed); an action can have any number.
    void bind(std::string_view action, InputBinding binding);
    void bind(std::string_view action, std::initializer_list<InputBinding> bindings);
    void unbind(std::string_view action);
    void unbind_all() { actions_.clear(); }
    bool has_action(std::string_view action) const { return find(action) != nullptr; }
    const std::vector<InputBinding>* bindings(std::string_view action) const;

    // Rebinding: the first key/button/stick pushed this frame, e.g. for "press a key for Jump" screens.
    std::optional<InputBinding> last_pressed() const { return last_pressed_; }
    // Readable name for prompts and settings screens: "Space", "Left mouse", "Gamepad a", "Gamepad leftx+".
    static std::string describe(const InputBinding& binding);
    // Save/restore bindings (readable JSON: {"jump": [{"key": "Space"}, {"button": "a"}]}). Loading replaces the
    // bindings of the actions it mentions and leaves others alone.
    json::Value save_bindings() const;
    void load_bindings(const json::Value& json);
    bool down(std::string_view action) const;
    bool pressed(std::string_view action) const;
    bool released(std::string_view action) const;
    // 0..1: analog for axes and triggers, 0 or 1 for buttons.
    f32 value(std::string_view action) const;
    // Two-axis input from four actions, length clamped to 1, with a radial dead zone.
    Vec2 vector(std::string_view neg_x, std::string_view pos_x, std::string_view neg_y, std::string_view pos_y,
                f32 dead_zone = 0.15f) const;

    // Called by the App each frame: begin_frame, handle_event for each event, end_events, then
    // begin_fixed_tick/end_fixed_tick around every fixed tick.
    void begin_frame();
    void end_events();
    void begin_fixed_tick();
    void end_fixed_tick();
    void set_context(Context c) { context_ = c; }
    void handle_event(const SDL_Event& event);
    // Dev UI captured the keyboard/mouse: games see nothing new until released.
    void set_blocked(bool keyboard, bool mouse) { block_keyboard_ = keyboard; block_mouse_ = mouse; }

private:
    static constexpr usize code_count = 1024;
    static constexpr u16 mouse_base = 512;
    static constexpr u16 pad_base = 576;
    static u16 key_code(Key k) { return static_cast<u16>(k) & 511; }
    static u16 mouse_code(MouseButton b) { return mouse_base + static_cast<u16>(b); }
    static u16 pad_code(int pad, GamepadButton b) { return static_cast<u16>(pad_base + pad * 32 + static_cast<int>(b)); }
    usize ctx() const { return static_cast<usize>(context_); }

    void set_down(u16 code, bool is_down);
    f32 binding_value(const InputBinding& b) const;
    bool binding_down(const InputBinding& b) const;
    bool binding_edge(const InputBinding& b, bool press) const;
    int pad_slot(u32 joystick_id) const;

    struct Action {
        std::string name;
        std::vector<InputBinding> bindings;
        std::array<bool, 2> axis_was_down{}; // per context, for axis edge detection
        std::array<bool, 2> axis_pressed{}, axis_released{};
    };
    const Action* find(std::string_view action) const;
    void update_axis_edges(usize context);

    std::bitset<code_count> down_;
    std::array<std::bitset<code_count>, 2> pressed_, released_;
    Vec2 mouse_, mouse_delta_;
    f32 wheel_ = 0;
    std::vector<Touch> touches_;
    std::string text_;
    std::array<SDL_Gamepad*, max_gamepads> pads_{};
    std::array<u32, max_gamepads> pad_ids_{};
    std::array<std::array<f32, 6>, max_gamepads> axes_{};
    std::unordered_map<u64, Action> actions_;
    Context context_ = Context::Frame;
    bool block_keyboard_ = false, block_mouse_ = false;
    std::optional<InputBinding> last_pressed_;
    Device last_device_ = Device::KeyboardMouse;
};

} // namespace ph
