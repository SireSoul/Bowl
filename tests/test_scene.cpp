#include "ph/app/engine.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ph;

namespace {

std::vector<std::string> events;

struct Named : Scene {
    std::string name;
    bool see_through = false;
    explicit Named(std::string n, bool transparent_ = false) : name(std::move(n)), see_through(transparent_) {}
    void enter(Engine&) override { events.push_back(name + ":enter"); }
    void exit(Engine&) override { events.push_back(name + ":exit"); }
    void pause(Engine&) override { events.push_back(name + ":pause"); }
    void resume(Engine&) override { events.push_back(name + ":resume"); }
    void update(Engine&, f32) override { events.push_back(name + ":update"); }
    void render(Engine&) override { events.push_back(name + ":render"); }
    bool transparent() const override { return see_through; }
};

} // namespace

TEST_CASE("scenes: push, pop, replace run lifecycle callbacks in order, at the end of the frame") {
    events.clear();
    Engine engine; // no window or GPU needed for scene logic
    SceneStack& s = engine.scenes();
    s.push(std::make_unique<Named>("title"));
    CHECK(s.empty()); // deferred
    s.apply_pending(engine);
    REQUIRE(s.size() == 1);
    s.push(std::make_unique<Named>("pause", true));
    s.apply_pending(engine);
    events.clear();

    s.update(engine, 0.016f); // only the top scene updates
    s.render(engine);         // the pause menu is transparent: both draw, bottom first
    CHECK(events == std::vector<std::string>{"pause:update", "title:render", "pause:render"});

    events.clear();
    s.pop();
    s.replace(std::make_unique<Named>("game"));
    s.apply_pending(engine);
    CHECK(events == std::vector<std::string>{"pause:exit", "title:resume", "title:exit", "game:enter"});
    CHECK(s.size() == 1);

    events.clear();
    s.shutdown(engine);
    CHECK(events == std::vector<std::string>{"game:exit"});
}

TEST_CASE("scenes: a fading replace waits until the screen is covered") {
    events.clear();
    Engine engine;
    SceneStack& s = engine.scenes();
    s.push(std::make_unique<Named>("a"));
    s.apply_pending(engine);
    events.clear();
    s.replace(std::make_unique<Named>("b"), 0.1f); // 0.1 s at the fallback 1/60 s per frame: 6 frames out
    s.apply_pending(engine);
    CHECK(s.transitioning());
    for (int i = 0; i < 4; ++i) s.apply_pending(engine);
    CHECK(events.empty()); // still fading out
    for (int i = 0; i < 4; ++i) s.apply_pending(engine);
    CHECK(events == std::vector<std::string>{"a:exit", "b:enter"});
    for (int i = 0; i < 10; ++i) s.apply_pending(engine); // fade back in
    CHECK_FALSE(s.transitioning());
    s.shutdown(engine);
}
