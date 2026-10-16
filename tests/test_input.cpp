#include "bowl/platform/input.hpp"
#include "bowl/platform/system.hpp"

#include <SDL3/SDL_events.h>
#include <doctest/doctest.h>

using namespace bowl;

namespace {
SDL_Event key_event(Key k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = static_cast<SDL_Scancode>(k);
    e.key.down = down;
    return e;
}
} // namespace

TEST_CASE("input: actions combine bindings") {
    Input input;
    input.bind("jump", {InputBinding::key(Key::Space), InputBinding::key(Key::W)});
    input.begin_frame();
    input.handle_event(key_event(Key::W, true));
    input.end_events();
    CHECK(input.down("jump"));
    CHECK(input.pressed("jump"));
    CHECK_FALSE(input.down("unknown action"));

    input.bind("jump", InputBinding::key(Key::Space)); // bind adds, even a duplicate
    CHECK(input.bindings("jump")->size() == 3);
    input.unbind_all();
    CHECK_FALSE(input.has_action("jump"));
}

TEST_CASE("input: a tap is seen once per frame and once per fixed tick") {
    Input input;
    input.bind("fire", InputBinding::key(Key::F));
    input.begin_frame();
    input.handle_event(key_event(Key::F, true));
    input.handle_event(key_event(Key::F, false)); // pressed and released within one frame
    input.end_events();

    input.set_context(Input::Context::Fixed);
    input.begin_fixed_tick();
    CHECK(input.pressed("fire"));
    CHECK_FALSE(input.down("fire"));
    input.end_fixed_tick();
    input.begin_fixed_tick();
    CHECK_FALSE(input.pressed("fire")); // second tick in the same frame: already consumed
    input.end_fixed_tick();

    input.set_context(Input::Context::Frame);
    CHECK(input.pressed("fire")); // frame code still sees it this frame
    input.begin_frame();
    input.end_events();
    CHECK_FALSE(input.pressed("fire"));
}

TEST_CASE("input: a press with no fixed tick this frame carries over to the next tick") {
    Input input;
    input.begin_frame();
    input.handle_event(key_event(Key::Space, true));
    input.end_events();
    // high frame rate: no fixed tick ran this frame
    input.begin_frame();
    input.end_events();
    input.set_context(Input::Context::Fixed);
    input.begin_fixed_tick();
    CHECK(input.pressed(Key::Space));
}

TEST_CASE("input: vector normalizes diagonals") {
    Input input;
    input.bind("left", InputBinding::key(Key::A));
    input.bind("right", InputBinding::key(Key::D));
    input.bind("up", InputBinding::key(Key::W));
    input.bind("down", InputBinding::key(Key::S));
    input.handle_event(key_event(Key::D, true));
    input.handle_event(key_event(Key::S, true));
    Vec2 v = input.vector("left", "right", "up", "down");
    CHECK(v.length() == doctest::Approx(1.0f));
    CHECK(v.x > 0);
    CHECK(v.y > 0);
}

TEST_CASE("platform: Steam Deck detection can be overridden") {
    bool detected = bowl::is_steam_deck(); // false on the machines tests run on, but don't rely on it
    bowl::set_steam_deck(true);
    CHECK(bowl::is_steam_deck());
    bowl::set_steam_deck(false);
    CHECK_FALSE(bowl::is_steam_deck());
    bowl::set_steam_deck(std::nullopt);
    CHECK(bowl::is_steam_deck() == detected);
}

TEST_CASE("input: touches last from the frame they go down to the frame they lift") {
    Input input;
    auto finger = [](Uint32 type, SDL_FingerID id) {
        SDL_Event e{};
        e.type = type;
        e.tfinger.fingerID = id;
        return e;
    };
    input.handle_event(finger(SDL_EVENT_FINGER_DOWN, 7));
    REQUIRE(input.touches().size() == 1);
    CHECK(input.touches()[0].id == 7);
    CHECK(input.touches()[0].pressed);
    CHECK(input.last_device() == Input::Device::Touch);

    input.begin_frame();
    CHECK_FALSE(input.touches()[0].pressed); // still down, no longer new
    input.handle_event(finger(SDL_EVENT_FINGER_DOWN, 8));
    input.handle_event(finger(SDL_EVENT_FINGER_UP, 7));
    REQUIRE(input.touches().size() == 2);
    CHECK(input.touches()[0].released);
    CHECK(input.touches()[1].pressed);

    input.begin_frame();
    REQUIRE(input.touches().size() == 1); // the lifted one is gone
    CHECK(input.touches()[0].id == 8);
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.handle_event(lost);
    CHECK(input.touches()[0].released);
}
