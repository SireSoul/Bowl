#include "ph/core/settings.hpp"
#include "ph/platform/input.hpp"

#include <SDL3/SDL_events.h>
#include <doctest/doctest.h>

#include <filesystem>

using namespace ph;
namespace fs = std::filesystem;

TEST_CASE("settings: dotted keys, typed values, atomic save and reload") {
    fs::path file = fs::temp_directory_path() / "ph_tests" / "settings" / "settings.json";
    fs::remove_all(file.parent_path());
    fs::create_directories(file.parent_path());
    {
        Settings s;
        CHECK_FALSE(s.load(file)); // first run
        CHECK(s.get("audio.music", 0.8f) == doctest::Approx(0.8f));
        s.set("audio.music", 0.25f);
        s.set("audio.mute", true);
        s.set("player.name", std::string("Ada"));
        s.set("window.size", Vec2{1600, 900});
        CHECK(s.dirty());
        REQUIRE(s.save());
        CHECK_FALSE(s.dirty());
        CHECK_FALSE(fs::exists(fs::path(file).concat(".tmp")));
    }
    Settings s;
    REQUIRE(s.load(file));
    CHECK(s.get("audio.music", 0.0f) == doctest::Approx(0.25f));
    CHECK(s.get("audio.mute", false));
    CHECK(s.get("player.name", std::string()) == "Ada");
    CHECK(s.get("window.size", Vec2{}) == Vec2{1600, 900});
    CHECK(s.get("audio.missing", 7) == 7);
    CHECK(s.get("player.name", 5) == 5); // wrong type: fallback
}

TEST_CASE("input bindings save as readable JSON and load back") {
    Input a;
    a.bind("jump", {InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
    a.bind("walk_left", {InputBinding::key(Key::A), InputBinding::axis(GamepadAxis::LeftX, -1)});
    a.bind("fire", InputBinding::mouse(MouseButton::Left));
    json::Value saved = a.save_bindings();
    CHECK(saved["jump"][0]["key"].as_string() == "Space");
    CHECK(saved["jump"][1]["button"].as_string() == "a");
    CHECK(saved["walk_left"][1]["axis"].as_string() == "leftx");

    Input b;
    b.bind("jump", InputBinding::key(Key::W)); // replaced by the loaded bindings
    b.bind("menu", InputBinding::key(Key::Escape)); // not in the file: kept
    b.load_bindings(saved);
    REQUIRE(b.bindings("jump"));
    CHECK(b.bindings("jump")->size() == 2);
    CHECK(Input::describe((*b.bindings("jump"))[0]) == "Space");
    CHECK(Input::describe((*b.bindings("walk_left"))[1]) == "Gamepad leftx-");
    CHECK(b.has_action("menu"));
}

TEST_CASE("input: last_pressed captures the first press of the frame for rebinding") {
    Input in;
    in.begin_frame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_K;
    e.key.down = true;
    in.handle_event(e);
    REQUIRE(in.last_pressed());
    CHECK(Input::describe(*in.last_pressed()) == "K");
    in.begin_frame();
    CHECK_FALSE(in.last_pressed());
}
