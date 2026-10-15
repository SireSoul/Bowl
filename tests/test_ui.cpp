#include "ph/ui/ui.hpp"

#include <SDL3/SDL_events.h>
#include <doctest/doctest.h>

using namespace ph;

namespace {

SDL_Event key(Key k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = static_cast<SDL_Scancode>(k);
    e.key.down = down;
    return e;
}
SDL_Event mouse_move(f32 x, f32 y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    return e;
}
SDL_Event mouse_button(bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = 1;
    e.button.down = down;
    return e;
}

// One UI frame: events first, then the widgets.
struct Harness {
    Input input;
    gfx::SpriteBatch batch;
    ui::Context ui;
    bool music = false;
    f32 volume = 0.5f;
    int start_clicks = 0, quit_clicks = 0;

    void frame(std::initializer_list<SDL_Event> events = {}) {
        input.begin_frame();
        for (const SDL_Event& e : events) input.handle_event(e);
        input.end_events();
        batch.begin(gfx::Camera2D::screen({800, 600}), gfx::SpriteSort::None);
        ui.begin(batch, input, {800, 600}, 1.0f / 60);
        ui.begin_panel("menu", {100, 100, 300, 0});
        if (ui.button("Start")) ++start_clicks;
        ui.checkbox("Music", music);
        ui.slider("Volume", volume, 0, 1);
        if (ui.button("Quit")) ++quit_clicks;
        ui.end_panel();
        ui.end();
    }
};

} // namespace

TEST_CASE("ui: keyboard focus moves through widgets and confirm activates") {
    Harness h;
    h.frame(); // first frame registers the widgets
    h.frame({key(Key::Down, true)}); // focus the first widget
    h.frame({key(Key::Down, false), key(Key::Enter, true)});
    CHECK(h.start_clicks == 1);
    h.frame({key(Key::Enter, false), key(Key::Down, true)}); // -> Music
    h.frame({key(Key::Down, false), key(Key::Space, true)});
    CHECK(h.music);
    h.frame({key(Key::Space, false), key(Key::Down, true)}); // -> Volume
    h.frame({key(Key::Down, false), key(Key::Right, true)});
    CHECK(h.volume == doctest::Approx(0.55f));
    h.frame({key(Key::Right, false), key(Key::Up, true)}); // wraps: Volume -> Music
    h.frame({key(Key::Up, false), key(Key::Up, true)});    // -> Start
    h.frame({key(Key::Up, false), key(Key::Up, true)});    // wraps -> Quit
    h.frame({key(Key::Up, false), key(Key::Enter, true)});
    CHECK(h.quit_clicks == 1);
    CHECK(h.start_clicks == 1);
}

TEST_CASE("ui: mouse clicks activate on release over the widget") {
    Harness h;
    h.frame();
    // Panel at (100,100), padding 14: first button spans y 114..148.
    h.frame({mouse_move(200, 130)});
    h.frame({mouse_button(true)});
    CHECK(h.start_clicks == 0); // not yet: activation happens on release
    CHECK(h.ui.wants_pointer());
    h.frame({mouse_button(false)});
    CHECK(h.start_clicks == 1);
    // Press on a button, release elsewhere: no click.
    h.frame({mouse_button(true)});
    h.frame({mouse_move(600, 500)});
    h.frame({mouse_button(false)});
    CHECK(h.start_clicks == 1);
}

TEST_CASE("ui: panels auto-size and record draws") {
    Harness h;
    h.frame();
    h.frame();
    CHECK(h.batch.size() > 8); // panel + widget frames + slider parts
}

TEST_CASE("ui: typewriter reveals, skips, and finishes") {
    ui::Typewriter tw;
    tw.start("Hi there"); // 7 visible characters
    tw.chars_per_second = 10;
    tw.update(0.3f);
    CHECK(tw.visible_glyphs() == 3);
    CHECK_FALSE(tw.done());
    tw.update(0.4f);
    CHECK(tw.done());
    tw.start("Next line");
    CHECK_FALSE(tw.done());
    tw.skip();
    CHECK(tw.done());
}
