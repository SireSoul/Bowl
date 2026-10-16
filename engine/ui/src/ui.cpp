#include "bowl/ui/ui.hpp"

#include "bowl/core/type_id.hpp"
#include "bowl/core/utf8.hpp"

#include <algorithm>

namespace bowl::ui {

namespace {

// "Save##slot2" shows "Save" but is a different widget than "Save##slot1".
std::string_view display(std::string_view label) {
    usize hash = label.find("##");
    return hash == std::string_view::npos ? label : label.substr(0, hash);
}

void bind_defaults(Input& in) {
    auto bind = [&](std::string_view action, std::initializer_list<InputBinding> b) {
        if (!in.has_action(action)) in.bind(action, b);
    };
    bind("ui_up", {InputBinding::key(Key::Up), InputBinding::key(Key::W), InputBinding::button(GamepadButton::DpadUp),
                   InputBinding::axis(GamepadAxis::LeftY, -1)});
    bind("ui_down", {InputBinding::key(Key::Down), InputBinding::key(Key::S), InputBinding::button(GamepadButton::DpadDown),
                     InputBinding::axis(GamepadAxis::LeftY, +1)});
    bind("ui_left", {InputBinding::key(Key::Left), InputBinding::key(Key::A), InputBinding::button(GamepadButton::DpadLeft),
                     InputBinding::axis(GamepadAxis::LeftX, -1)});
    bind("ui_right", {InputBinding::key(Key::Right), InputBinding::key(Key::D),
                      InputBinding::button(GamepadButton::DpadRight), InputBinding::axis(GamepadAxis::LeftX, +1)});
    bind("ui_confirm", {InputBinding::key(Key::Enter), InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
    bind("ui_back", {InputBinding::key(Key::Escape), InputBinding::button(GamepadButton::East)});
}

} // namespace

bool Typewriter::done() const {
    i32 visible = 0;
    for (usize i = 0; i < text.size();) {
        u32 cp = utf8::next(text, i);
        if (cp != ' ' && cp != '\n' && cp != '\t') ++visible;
    }
    return shown >= static_cast<f32>(visible);
}

void Context::begin(gfx::SpriteBatch& batch, Input& input, Vec2 screen_size, f32 dt, f32 pixel_density) {
    batch_ = &batch;
    input_ = &input;
    screen_ = screen_size;
    dt_ = dt;
    density_ = pixel_density;
    panels_.clear();
    last_focus_order_.swap(focus_order_);
    focus_order_.clear();
    pointer_over_ui_ = false;
    bind_defaults(input);

    Vec2 mouse = input.mouse_position();
    mouse_moved_ = (mouse - last_mouse_).length_squared() > 0.25f;
    last_mouse_ = mouse;

    // Keyboard / gamepad focus moves through last frame's widgets in order.
    auto move = [&](int step) {
        if (last_focus_order_.empty()) return;
        auto it = std::find(last_focus_order_.begin(), last_focus_order_.end(), focused_);
        if (it == last_focus_order_.end()) {
            focused_ = step > 0 ? last_focus_order_.front() : last_focus_order_.back();
            return;
        }
        auto i = static_cast<isize>(it - last_focus_order_.begin()) + step;
        auto n = static_cast<isize>(last_focus_order_.size());
        focused_ = last_focus_order_[static_cast<usize>((i % n + n) % n)];
    };
    if (input.pressed("ui_down")) move(+1);
    if (input.pressed("ui_up")) move(-1);
    confirm_ = input.pressed("ui_confirm");
    back_ = input.pressed("ui_back");
    left_ = input.pressed("ui_left");
    right_ = input.pressed("ui_right");
}

void Context::end() {
    if (input_ && input_->released(MouseButton::Left)) active_ = 0;
    batch_ = nullptr;
}

u64 Context::make_id(std::string_view label) const {
    u64 parent = panels_.empty() ? 0 : panels_.back().id;
    return fnv1a(label) ^ (parent * 0x9E3779B97F4A7C15ull);
}

Rect Context::next_rect(f32 height) {
    if (panels_.empty()) panels_.push_back({0, {0, 0, screen_.x, screen_.y}, style.padding}); // implicit root
    Panel& p = panels_.back();
    Rect r{p.rect.x + style.padding, p.cursor_y, p.rect.w - 2 * style.padding, height};
    p.cursor_y += height + style.spacing;
    return r;
}

Context::WidgetState Context::interact(u64 id, Rect r) {
    focus_order_.push_back(id);
    WidgetState s{};
    s.hovered = r.contains(input_->mouse_position());
    if (s.hovered) {
        pointer_over_ui_ = true;
        if (mouse_moved_) focused_ = id;
        if (input_->pressed(MouseButton::Left)) {
            active_ = id;
            focused_ = id;
        }
    }
    s.focused = focused_ == id;
    s.pressed = active_ == id && input_->down(MouseButton::Left);
    s.activated = (active_ == id && s.hovered && input_->released(MouseButton::Left)) || (s.focused && confirm_);
    return s;
}

void Context::frame(Rect r, Color fill, Color border) {
    if (style.panel_texture) {
        batch_->draw_nine_slice(*style.panel_texture, r, style.panel_insets, fill);
    } else {
        batch_->draw_rect(r, fill);
        batch_->draw_outline(r, border, 1.5f);
    }
}

void Context::text(std::string_view s, Vec2 at, Color color, f32 size, gfx::TextAlign align, f32 max_width, i32 max_glyphs) {
    if (!style.font) return;
    gfx::TextStyle ts;
    ts.size = size;
    ts.color = color;
    ts.align = align;
    ts.max_width = max_width;
    ts.raster_scale = density_;
    ts.max_glyphs = max_glyphs;
    gfx::draw_text(*batch_, *style.font, s, at, ts);
}

bool Context::begin_panel(std::string_view id, Rect rect) {
    u64 pid = fnv1a(id);
    if (rect.h <= 0) {
        auto it = panel_heights_.find(pid);
        rect.h = it == panel_heights_.end() ? 0 : it->second;
    }
    if (rect.h > 0) frame(rect, style.panel, style.border);
    if (rect.contains(input_->mouse_position())) pointer_over_ui_ = true;
    panels_.push_back({pid, rect, rect.y + style.padding});
    return true;
}

void Context::end_panel() {
    if (panels_.empty()) return;
    Panel p = panels_.back();
    panels_.pop_back();
    panel_heights_[p.id] = p.cursor_y - style.spacing + style.padding - p.rect.y;
}

void Context::label(std::string_view s, Color color, f32 size) {
    if (size <= 0) size = style.font_size;
    if (color.a <= 0) color = style.text;
    f32 width = (panels_.empty() ? screen_.x : panels_.back().rect.w) - 2 * style.padding;
    f32 height = size * 1.3f;
    if (style.font) {
        gfx::TextStyle ts;
        ts.size = size;
        ts.max_width = width;
        ts.raster_scale = density_;
        height = gfx::measure_text(*style.font, s, ts).y;
    }
    Rect r = next_rect(height);
    text(s, r.position(), color, size, gfx::TextAlign::Left, r.w);
}

bool Context::button(std::string_view label) {
    u64 id = make_id(label);
    Rect r = next_rect(style.widget_height);
    WidgetState s = interact(id, r);
    Color fill = s.pressed ? style.widget_active : (s.hovered || s.focused) ? style.widget_hover : style.widget;
    frame(r, fill, s.focused ? style.accent : style.border);
    f32 size = style.font_size;
    text(display(label), {r.center().x, r.y + (r.h - size * 1.17f) * 0.5f}, style.text, size, gfx::TextAlign::Center);
    return s.activated;
}

bool Context::checkbox(std::string_view label, bool& value) {
    u64 id = make_id(label);
    Rect r = next_rect(style.widget_height);
    WidgetState s = interact(id, r);
    if (s.activated) value = !value;
    frame(r, (s.hovered || s.focused) ? style.widget_hover : style.widget, s.focused ? style.accent : style.border);
    f32 box = r.h - 12;
    Rect check{r.right() - box - 6, r.y + 6, box, box};
    batch_->draw_outline(check, style.text_dim, 2);
    if (value) batch_->draw_rect({check.x + 5, check.y + 5, check.w - 10, check.h - 10}, style.accent);
    text(display(label), {r.x + 10, r.y + (r.h - style.font_size * 1.17f) * 0.5f}, style.text, style.font_size);
    return s.activated;
}

bool Context::slider(std::string_view label, f32& value, f32 min, f32 max) {
    u64 id = make_id(label);
    Rect r = next_rect(style.widget_height);
    WidgetState s = interact(id, r);
    f32 before = value;
    Rect track{r.x + r.w * 0.45f, r.y + r.h * 0.5f - 3, r.w * 0.55f - 10, 6};
    if (s.pressed) value = lerp(min, max, saturate((input_->mouse_position().x - track.x) / track.w));
    if (s.focused) {
        f32 step = (max - min) * 0.05f;
        if (left_) value -= step;
        if (right_) value += step;
    }
    value = std::clamp(value, min, max);
    frame(r, (s.hovered || s.focused) ? style.widget_hover : style.widget, s.focused ? style.accent : style.border);
    text(display(label), {r.x + 10, r.y + (r.h - style.font_size * 1.17f) * 0.5f}, style.text, style.font_size);
    f32 t = max > min ? (value - min) / (max - min) : 0;
    batch_->draw_rect(track, style.border);
    batch_->draw_rect({track.x, track.y, track.w * t, track.h}, style.accent);
    batch_->draw_rect({track.x + track.w * t - 4, r.y + 7, 8, r.h - 14}, style.text);
    return value != before;
}

void Context::progress(f32 fraction, Color fill) {
    Rect r = next_rect(12);
    batch_->draw_rect(r, style.widget);
    batch_->draw_rect({r.x, r.y, r.w * saturate(fraction), r.h}, fill.a > 0 ? fill : style.accent);
}

void Context::space(f32 height) { next_rect(std::max(0.0f, height - style.spacing)); }

bool Context::dialogue(std::string_view speaker, Typewriter& tw) {
    tw.update(dt_);
    Rect box{screen_.x * 0.1f, screen_.y - 190, screen_.x * 0.8f, 160};
    frame(box, style.panel, style.border);
    f32 pad = style.padding + 4;
    f32 y = box.y + pad;
    if (!speaker.empty()) {
        text(speaker, {box.x + pad, y}, style.accent, style.font_size * 1.1f);
        y += style.font_size * 1.6f;
    }
    text(tw.text, {box.x + pad, y}, style.text, style.font_size, gfx::TextAlign::Left, box.w - 2 * pad, tw.visible_glyphs());
    bool advance = false;
    bool clicked = box.contains(input_->mouse_position()) && input_->pressed(MouseButton::Left);
    if (box.contains(input_->mouse_position())) pointer_over_ui_ = true;
    if (confirm_ || clicked) {
        if (!tw.done()) tw.skip();
        else advance = true;
    }
    if (tw.done()) { // a blinking "more" marker
        f32 blink = 0.5f + 0.5f * std::sin(blink_time_ * 6.0f);
        text(">", {box.right() - pad - 10, box.bottom() - pad - style.font_size}, style.accent.with_alpha(blink),
             style.font_size);
    }
    blink_time_ += dt_;
    return advance;
}

} // namespace bowl::ui
