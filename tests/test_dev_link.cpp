#include "bowl/app/dev_link.hpp"
#include "bowl/core/file.hpp"
#include "bowl/ecs/hierarchy.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace bowl;
using namespace bowl::devlink;

namespace dev_link_test {

struct Health {
    i32 hp = 0;
};
BOWL_COMPONENT(Health, hp)

struct Speed {
    f32 value = 0;
};
BOWL_COMPONENT(Speed, value)

// A game's side and an editor's, on this computer, stepped on a made-up clock (packets still go through real sockets).
struct Link {
    ecs::World world;
    ecs::Schedule fixed, frame;
    TimeControl time{};
    int time_calls = 0;
    std::unique_ptr<DevLink> game;
    DevLinkClient editor;
    f64 now = 0;

    Link() {
        ecs::register_components<ecs::Name, Health, Speed>();
        fixed.add("heal", [](ecs::SystemContext&) {}).writes<Health>();
        fixed.add("hurt", [](ecs::SystemContext&) {}).reads<Health>();
        frame.add("draw", [](ecs::SystemContext&) {}).reads<Speed>();
        Sources s;
        s.game = "Test game";
        s.world = &world;
        s.fixed = &fixed;
        s.frame = &frame;
        s.stats = [this] { return Frame{.ms = 16, .entities = static_cast<u32>(world.entity_count())}; };
        s.time = [this](const TimeControl& t) { time = t, ++time_calls; };
        s.time_state = [this] { return time; };
        game = std::make_unique<DevLink>(std::move(s));
        REQUIRE(game->start(false));
    }
    void tick() {
        game->update(now);
        editor.update(now);
    }
    template <class Done> bool step(f64 seconds, Done done) {
        for (f64 end = now + seconds; now < end; now += 0.01) {
            tick();
            if (done()) return true;
        }
        for (auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
             std::chrono::steady_clock::now() < give_up;) {
            tick();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

} // namespace dev_link_test

using namespace dev_link_test;

TEST_CASE("dev link: the editor sees the game's schedules, frames and entities, and changes them") {
    Link link;
    ecs::Entity hero = link.world.create(ecs::Name{"hero"}, Health{10}, Speed{2.5f});
    link.world.create(ecs::Name{"rock"});
    for (int i = 0; i < 30; ++i) link.world.create(Health{i});

    REQUIRE(link.editor.connect(link.game->port(), link.game->key()));
    REQUIRE(link.step(5, [&] { return link.editor.connected() && link.editor.hello() && link.editor.schedules(); }));
    CHECK(link.editor.hello()->game == "Test game");
    const Schedules& s = *link.editor.schedules();
    REQUIRE(s.fixed.size() == 2);
    CHECK(s.fixed[1].why_new_batch.find("heal writes") != std::string::npos);
    REQUIRE(s.frame.size() == 1);
    CHECK(link.step(2, [&] { return !link.editor.frames().empty(); }));
    CHECK(link.editor.frames().back().entities == 32);

    // A page of entities, then only those matching a filter.
    link.editor.list_entities({.from = 0, .count = 10});
    REQUIRE(link.step(2, [&] { return link.editor.page().has_value(); }));
    CHECK(link.editor.page()->total == 32);
    CHECK(link.editor.page()->rows.size() == 10);
    link.editor.list_entities({.from = 0, .count = 10, .filter = "HER"});
    REQUIRE(link.step(2, [&] { return link.editor.page() && link.editor.page()->total == 1; }));
    CHECK(link.editor.page()->rows[0].name == "hero");

    // Watching one: its components, as JSON; changing one changes the game's.
    link.editor.watch(hero.index, hero.generation);
    REQUIRE(link.step(2, [&] { return link.editor.detail().has_value(); }));
    const EntityDetail& d = *link.editor.detail();
    CHECK(d.alive);
    auto health = std::find_if(d.components.begin(), d.components.end(),
                               [](const ComponentJson& c) { return c.name.find("Health") != std::string::npos; });
    REQUIRE(health != d.components.end());
    CHECK(health->editable);
    CHECK(health->json.find("10") != std::string::npos);
    link.editor.set_component({hero.index, hero.generation, health->name, R"({"hp": 99})"});
    CHECK(link.step(2, [&] { return link.world.get<Health>(hero).hp == 99; }));
    CHECK(link.step(2, [&] {
        for (const auto& c : link.editor.detail()->components)
            if (c.name == health->name) return c.json.find("99") != std::string::npos;
        return false;
    }));

    // Systems off and on, and time.
    link.editor.set_system_enabled({"fixed", "hurt", false});
    CHECK(link.step(2, [&] { return !link.fixed.systems()[1]->enabled; }));
    CHECK(link.step(2, [&] { return !link.editor.schedules()->fixed[1].enabled; }));
    link.editor.time({.paused = true, .step = true, .time_scale = 0.5f});
    CHECK(link.step(2, [&] { return link.time_calls > 0; }));
    CHECK(link.time.paused);
    CHECK(link.time.time_scale == 0.5f);
    CHECK(link.step(2, [&] { return link.editor.frames().back().paused; }));

    // A change to a dead entity, or to a component it doesn't have, does nothing.
    link.world.destroy(hero);
    link.editor.set_component({hero.index, hero.generation, health->name, R"({"hp": 1})"});
    CHECK(link.step(2, [&] { return link.editor.detail() && !link.editor.detail()->alive; }));
}

TEST_CASE("dev link: without the game's key, nothing connects") {
    Link link;
    REQUIRE(link.editor.connect(link.game->port(), "not the key"));
    link.step(2, [&] { return link.editor.connected(); });
    CHECK_FALSE(link.editor.connected());
    CHECK(link.game->connections() == 0);
}

TEST_CASE("dev link: running games are found by their files, and a dead game's file goes") {
    namespace fs = std::filesystem;
    fs::create_directories(folder());
    fs::path stale = folder() / "999999999.json";
    std::string text = R"({"game": "gone", "port": 4000, "key": "k", "pid": 999999999})";
    REQUIRE(write_file_bytes(stale, text.data(), text.size()));
    Link link;
    for (const GameEntry& g : running_games()) CHECK(g.game != "gone");
    CHECK_FALSE(fs::exists(stale));
}
