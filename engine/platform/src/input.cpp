#include "bowl/platform/input.hpp"

#include "bowl/core/log.hpp"
#include "bowl/core/type_id.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace bowl {

Input::Input() = default;

Input::~Input() {
    for (SDL_Gamepad* pad : pads_)
        if (pad) SDL_CloseGamepad(pad);
}

void Input::set_down(u16 code, bool is_down) {
    if (code >= code_count || down_.test(code) == is_down) return;
    down_.set(code, is_down);
    if (is_down && !last_pressed_) {
        if (code < 256) last_pressed_ = InputBinding{InputBinding::Kind::Key, static_cast<u8>(code), -1, 1, 0.5f};
        else if (code < mouse_base) {} // media keys etc.: not bindable
        else if (code < pad_base) last_pressed_ = InputBinding::mouse(static_cast<MouseButton>(code - mouse_base));
        else last_pressed_ = InputBinding::button(static_cast<GamepadButton>((code - pad_base) % 32));
    }
    for (usize c = 0; c < 2; ++c) {
        if (is_down) pressed_[c].set(code);
        else released_[c].set(code);
    }
}

int Input::pad_slot(u32 joystick_id) const {
    for (int i = 0; i < max_gamepads; ++i)
        if (pads_[i] && pad_ids_[i] == joystick_id) return i;
    return -1;
}

void Input::begin_frame() {
    last_pressed_.reset();
    pressed_[0].reset();
    released_[0].reset();
    mouse_delta_ = {};
    wheel_ = 0;
    text_.clear();
    std::erase_if(touches_, [](const Touch& t) { return t.released; });
    for (Touch& t : touches_) t.pressed = false;
}

void Input::end_events() { update_axis_edges(0); }

void Input::begin_fixed_tick() { update_axis_edges(1); }

void Input::end_fixed_tick() {
    pressed_[1].reset();
    released_[1].reset();
    for (auto& [id, a] : actions_) a.axis_pressed[1] = a.axis_released[1] = false;
}

void Input::update_axis_edges(usize context) {
    for (auto& [id, a] : actions_) {
        bool now = false;
        for (const InputBinding& b : a.bindings)
            if (b.kind == InputBinding::Kind::GamepadAxis && binding_down(b)) now = true;
        a.axis_pressed[context] = now && !a.axis_was_down[context];
        a.axis_released[context] = !now && a.axis_was_down[context];
        a.axis_was_down[context] = now;
    }
}

void Input::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.repeat) break;
        // Releases always go through, so keys never get stuck when dev UI grabs focus mid-press.
        if (e.type == SDL_EVENT_KEY_DOWN && block_keyboard_) break;
        if (e.type == SDL_EVENT_KEY_DOWN) last_device_ = Device::KeyboardMouse;
        set_down(static_cast<u16>(e.key.scancode & 511), e.type == SDL_EVENT_KEY_DOWN);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && block_mouse_) break;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.which != SDL_TOUCH_MOUSEID) last_device_ = Device::KeyboardMouse;
        set_down(static_cast<u16>(mouse_base + e.button.button), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_ = {e.motion.x, e.motion.y};
        if (!block_mouse_) mouse_delta_ += {e.motion.xrel, e.motion.yrel};
        // A real move, not the jiggle of a bumped desk (or a finger: that's a touch).
        if (!block_mouse_ && e.motion.which != SDL_TOUCH_MOUSEID && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 4)
            last_device_ = Device::KeyboardMouse;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        // Finger positions come as 0..1 across the window.
        int w = 0, h = 0;
        if (SDL_Window* window = SDL_GetWindowFromID(e.tfinger.windowID)) SDL_GetWindowSize(window, &w, &h);
        Vec2 at{e.tfinger.x * static_cast<f32>(w), e.tfinger.y * static_cast<f32>(h)};
        auto it = std::find_if(touches_.begin(), touches_.end(), [&](const Touch& t) { return t.id == e.tfinger.fingerID && !t.released; });
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            last_device_ = Device::Touch;
            touches_.push_back({e.tfinger.fingerID, at, at, true, false});
        } else if (it != touches_.end()) {
            it->position = at;
            if (e.type != SDL_EVENT_FINGER_MOTION) it->released = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!block_mouse_) wheel_ += e.wheel.y;
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!block_keyboard_) text_ += e.text.text;
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        if (pad_slot(e.gdevice.which) >= 0) break;
        for (int i = 0; i < max_gamepads; ++i) {
            if (pads_[i]) continue;
            pads_[i] = SDL_OpenGamepad(e.gdevice.which);
            if (pads_[i]) {
                pad_ids_[i] = e.gdevice.which;
                info("input: gamepad {} connected: {}", i, SDL_GetGamepadName(pads_[i]));
            }
            break;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int slot = pad_slot(e.gdevice.which);
        if (slot < 0) break;
        for (int b = 0; b < 32; ++b) set_down(static_cast<u16>(pad_base + slot * 32 + b), false);
        axes_[slot] = {};
        SDL_CloseGamepad(pads_[slot]);
        pads_[slot] = nullptr;
        info("input: gamepad {} disconnected", slot);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int slot = pad_slot(e.gbutton.which);
        if (slot >= 0 && e.gbutton.button < 32)
            set_down(static_cast<u16>(pad_base + slot * 32 + e.gbutton.button), e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (slot >= 0 && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) last_device_ = Device::Gamepad;
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int slot = pad_slot(e.gaxis.which);
        if (slot >= 0 && e.gaxis.axis < 6) {
            f32 v = std::clamp(e.gaxis.value / 32767.0f, -1.0f, 1.0f);
            if (std::abs(v) > 0.5f) last_device_ = Device::Gamepad; // past any drift
            if (!last_pressed_ && std::abs(v) > 0.6f && std::abs(axes_[slot][e.gaxis.axis]) <= 0.6f)
                last_pressed_ = InputBinding::axis(static_cast<GamepadAxis>(e.gaxis.axis), v > 0 ? 1.0f : -1.0f);
            axes_[slot][e.gaxis.axis] = v;
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Release everything so nothing stays held while the window is in the background.
        for (usize code = 0; code < code_count; ++code)
            if (down_.test(code)) set_down(static_cast<u16>(code), false);
        for (Touch& t : touches_) t.released = true;
        break;
    default: break;
    }
}

f32 Input::axis(GamepadAxis a, int pad) const {
    if (pad < 0 || pad >= max_gamepads) return 0;
    return axes_[pad][static_cast<usize>(a)];
}

void Input::rumble(int pad, f32 low, f32 high, u32 milliseconds) {
    if (!gamepad_connected(pad)) return;
    auto to16 = [](f32 v) { return static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    SDL_RumbleGamepad(pads_[pad], to16(low), to16(high), milliseconds);
}

// Actions

void Input::bind(std::string_view action, InputBinding binding) {
    Action& a = actions_[fnv1a(action)];
    a.name = std::string(action);
    a.bindings.push_back(binding);
}

void Input::bind(std::string_view action, std::initializer_list<InputBinding> bindings) {
    for (const InputBinding& b : bindings) bind(action, b);
}

void Input::unbind(std::string_view action) { actions_.erase(fnv1a(action)); }

const Input::Action* Input::find(std::string_view action) const {
    auto it = actions_.find(fnv1a(action));
    return it == actions_.end() ? nullptr : &it->second;
}

f32 Input::binding_value(const InputBinding& b) const {
    switch (b.kind) {
    case InputBinding::Kind::Key: return down_.test(b.code) ? 1.0f : 0.0f;
    case InputBinding::Kind::Mouse: return down_.test(mouse_base + b.code) ? 1.0f : 0.0f;
    case InputBinding::Kind::GamepadButton: {
        for (int p = 0; p < max_gamepads; ++p)
            if ((b.gamepad < 0 || b.gamepad == p) && down_.test(pad_base + p * 32 + b.code)) return 1.0f;
        return 0.0f;
    }
    case InputBinding::Kind::GamepadAxis: {
        f32 best = 0;
        for (int p = 0; p < max_gamepads; ++p)
            if (b.gamepad < 0 || b.gamepad == p) best = std::max(best, axes_[p][b.code] * b.direction);
        return best;
    }
    }
    return 0;
}

bool Input::binding_down(const InputBinding& b) const {
    if (b.kind == InputBinding::Kind::GamepadAxis) return binding_value(b) >= b.threshold;
    return binding_value(b) > 0;
}

bool Input::binding_edge(const InputBinding& b, bool press) const {
    const auto& edges = press ? pressed_[ctx()] : released_[ctx()];
    switch (b.kind) {
    case InputBinding::Kind::Key: return edges.test(b.code);
    case InputBinding::Kind::Mouse: return edges.test(mouse_base + b.code);
    case InputBinding::Kind::GamepadButton:
        for (int p = 0; p < max_gamepads; ++p)
            if ((b.gamepad < 0 || b.gamepad == p) && edges.test(pad_base + p * 32 + b.code)) return true;
        return false;
    case InputBinding::Kind::GamepadAxis: return false; // handled per action
    }
    return false;
}

bool Input::down(std::string_view action) const {
    const Action* a = find(action);
    if (!a) return false;
    return std::any_of(a->bindings.begin(), a->bindings.end(), [&](const InputBinding& b) { return binding_down(b); });
}

bool Input::pressed(std::string_view action) const {
    const Action* a = find(action);
    if (!a) return false;
    if (a->axis_pressed[ctx()]) return true;
    return std::any_of(a->bindings.begin(), a->bindings.end(), [&](const InputBinding& b) { return binding_edge(b, true); });
}

bool Input::released(std::string_view action) const {
    const Action* a = find(action);
    if (!a) return false;
    if (a->axis_released[ctx()]) return true;
    return std::any_of(a->bindings.begin(), a->bindings.end(), [&](const InputBinding& b) { return binding_edge(b, false); });
}

f32 Input::value(std::string_view action) const {
    const Action* a = find(action);
    if (!a) return 0;
    f32 best = 0;
    for (const InputBinding& b : a->bindings) best = std::max(best, binding_value(b));
    return std::min(best, 1.0f);
}

Vec2 Input::vector(std::string_view neg_x, std::string_view pos_x, std::string_view neg_y, std::string_view pos_y,
                   f32 dead_zone) const {
    Vec2 v{value(pos_x) - value(neg_x), value(pos_y) - value(neg_y)};
    f32 len = v.length();
    if (len < dead_zone) return {};
    if (len > 1.0f) return v / len;
    // Rescale so output starts at 0 just past the dead zone.
    return v / len * ((len - dead_zone) / (1.0f - dead_zone));
}

} // namespace bowl

namespace bowl {

const std::vector<InputBinding>* Input::bindings(std::string_view action) const {
    const Action* a = find(action);
    return a ? &a->bindings : nullptr;
}

std::string Input::describe(const InputBinding& b) {
    switch (b.kind) {
    case InputBinding::Kind::Key: {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code));
        return name && *name ? name : "Key " + std::to_string(b.code);
    }
    case InputBinding::Kind::Mouse: {
        static const char* names[] = {"", "Left mouse", "Middle mouse", "Right mouse", "Mouse 4", "Mouse 5"};
        return b.code < std::size(names) ? names[b.code] : "Mouse " + std::to_string(b.code);
    }
    case InputBinding::Kind::GamepadButton: {
        const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b.code));
        return std::string("Gamepad ") + (name ? name : "?");
    }
    case InputBinding::Kind::GamepadAxis: {
        const char* name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(b.code));
        return std::string("Gamepad ") + (name ? name : "?") + (b.direction > 0 ? "+" : "-");
    }
    }
    return "?";
}

json::Value Input::save_bindings() const {
    json::Value out;
    out.make_object();
    // Sorted by name so the file is stable.
    std::vector<const Action*> sorted;
    for (const auto& [id, a] : actions_) sorted.push_back(&a);
    std::sort(sorted.begin(), sorted.end(), [](const Action* x, const Action* y) { return x->name < y->name; });
    for (const Action* a : sorted) {
        json::Value list;
        list.make_array();
        for (const InputBinding& b : a->bindings) {
            json::Value j;
            switch (b.kind) {
            case InputBinding::Kind::Key: j.set("key", SDL_GetScancodeName(static_cast<SDL_Scancode>(b.code))); break;
            case InputBinding::Kind::Mouse: j.set("mouse", static_cast<int>(b.code)); break;
            case InputBinding::Kind::GamepadButton:
                j.set("button", SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b.code)));
                break;
            case InputBinding::Kind::GamepadAxis:
                j.set("axis", SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(b.code)));
                j.set("direction", b.direction);
                j.set("threshold", b.threshold);
                break;
            }
            if (b.gamepad >= 0) j.set("gamepad", static_cast<int>(b.gamepad));
            list.push(std::move(j));
        }
        out.set(a->name, std::move(list));
    }
    return out;
}

void Input::load_bindings(const json::Value& json) {
    for (const auto& [name, list] : json.object()) {
        std::vector<InputBinding> parsed;
        for (const json::Value& j : list.array()) {
            InputBinding b;
            b.gamepad = static_cast<i8>(j["gamepad"].as_int(-1));
            if (j.contains("key")) {
                SDL_Scancode sc = SDL_GetScancodeFromName(std::string(j["key"].as_string()).c_str());
                if (sc == SDL_SCANCODE_UNKNOWN) continue;
                b.kind = InputBinding::Kind::Key;
                b.code = static_cast<u8>(sc);
            } else if (j.contains("mouse")) {
                b.kind = InputBinding::Kind::Mouse;
                b.code = static_cast<u8>(j["mouse"].as_int(1));
            } else if (j.contains("button")) {
                SDL_GamepadButton gb = SDL_GetGamepadButtonFromString(std::string(j["button"].as_string()).c_str());
                if (gb == SDL_GAMEPAD_BUTTON_INVALID) continue;
                b.kind = InputBinding::Kind::GamepadButton;
                b.code = static_cast<u8>(gb);
            } else if (j.contains("axis")) {
                SDL_GamepadAxis ga = SDL_GetGamepadAxisFromString(std::string(j["axis"].as_string()).c_str());
                if (ga == SDL_GAMEPAD_AXIS_INVALID) continue;
                b.kind = InputBinding::Kind::GamepadAxis;
                b.code = static_cast<u8>(ga);
                b.direction = j["direction"].as_float(1) >= 0 ? 1.0f : -1.0f;
                b.threshold = j["threshold"].as_float(0.5f);
            } else {
                continue;
            }
            parsed.push_back(b);
        }
        unbind(name);
        for (const InputBinding& b : parsed) bind(name, b);
    }
}

} // namespace bowl
