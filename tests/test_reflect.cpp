#include "ph/ecs/ecs.hpp"

#include <doctest/doctest.h>

using namespace ph;
using namespace ph::ecs;

namespace game {

enum class Faction : u8 { Wild, Town, Guild };

struct Stats {
    i32 strength = 1;
    f32 speed = 2.5f;
};
PH_REFLECT(Stats, strength, speed)

struct Hero {
    std::string name = "nobody";
    i32 level = 1;
    bool alive = true;
    Faction faction = Faction::Wild;
    Vec2 position;
    Color tint = Color::white();
    Stats stats;
    std::vector<i32> inventory;
    std::optional<f32> buff;
    Entity companion; // a reference to another entity
};
PH_REFLECT(Hero, name, level, alive, faction, position, tint, stats, inventory, buff, companion)

struct Asleep {}; // tag: savable without reflection

struct Cache { // not reflected: never saved
    int scratch = 0;
};

} // namespace game

using namespace game;

TEST_CASE("reflect: fields are visited in order with names") {
    static_assert(reflect::Reflected<Hero>);
    static_assert(!reflect::Reflected<Cache>);
    static_assert(reflect::field_count<Hero>() == 10);
    Stats s{7, 1.5f};
    std::vector<std::string> names;
    reflect::for_each_field(s, [&](std::string_view name, auto& value) {
        names.emplace_back(name);
        value = static_cast<std::remove_reference_t<decltype(value)>>(value * 2);
    });
    CHECK(names == std::vector<std::string>{"strength", "speed"});
    CHECK(s.strength == 14);
    CHECK(s.speed == doctest::Approx(3.0f));
}

TEST_CASE("serialize: values round-trip and missing fields keep defaults") {
    serial::Context ctx;
    Hero h;
    h.name = "Ada";
    h.level = 12;
    h.faction = Faction::Guild;
    h.position = {3, -4};
    h.tint = Color::hex(0x336699);
    h.stats = {9, 4.25f};
    h.inventory = {1, 2, 3};
    h.buff = 0.5f;
    json::Value j = serial::to_json(h, ctx);
    CHECK(j["name"].as_string() == "Ada");
    CHECK(j["stats"]["speed"].as_number() == doctest::Approx(4.25));
    CHECK(j["position"][1].as_number() == -4);

    Hero back;
    REQUIRE(serial::from_json(j, back, ctx));
    CHECK(back.name == "Ada");
    CHECK(back.level == 12);
    CHECK(back.faction == Faction::Guild);
    CHECK(back.position == Vec2{3, -4});
    CHECK(back.tint == h.tint);
    CHECK(back.stats.strength == 9);
    CHECK(back.inventory == std::vector<i32>{1, 2, 3});
    REQUIRE(back.buff);
    CHECK(*back.buff == doctest::Approx(0.5f));

    // An old save without newer fields: those keep their defaults.
    Hero partial;
    REQUIRE(serial::from_json(json::parse(R"({"name": "Old", "unknown_field": 1})").value, partial, ctx));
    CHECK(partial.name == "Old");
    CHECK(partial.level == 1);

    // Wrong types are reported and skipped.
    Hero wrong;
    CHECK_FALSE(serial::from_json(json::parse(R"({"level": "high"})").value, wrong, ctx));
    CHECK(wrong.level == 1);
    CHECK_FALSE(ctx.warnings.empty());
}

TEST_CASE("world io: entities, references, tags and unsavable components round-trip") {
    register_components<Hero, Asleep, Cache>();
    World a;
    Entity ada = a.create(Hero{.name = "Ada", .level = 3});
    Entity bo = a.create(Hero{.name = "Bo"}, Asleep{}, Cache{42});
    a.get<Hero>(ada).companion = bo;
    a.get<Hero>(bo).companion = ada; // a cycle
    a.create(Cache{}); // nothing savable: not saved

    serial::Context ctx;
    json::Value doc = save_world(a, ctx);
    CHECK(doc["entities"].size() == 2);
    bool warned_cache = false;
    for (auto& w : ctx.warnings) warned_cache = warned_cache || w.find("Cache") != std::string::npos;
    CHECK(warned_cache);

    // Load into a world that already has entities: ids differ, references must follow.
    World b;
    b.create();
    b.create();
    serial::Context load_ctx;
    auto loaded = load_entities(b, json::parse(json::stringify(doc)).value, load_ctx);
    REQUIRE(loaded.size() == 2);
    Entity ada2 = loaded[0], bo2 = loaded[1];
    CHECK(b.get<Hero>(ada2).name == "Ada");
    CHECK(b.get<Hero>(ada2).companion == bo2);
    CHECK(b.get<Hero>(bo2).companion == ada2);
    CHECK(b.has<Asleep>(bo2));
    CHECK_FALSE(b.has<Cache>(bo2));
    CHECK(ada2 != ada); // fresh ids
}

TEST_CASE("world io: prefabs and unknown components") {
    World w;
    serial::Context ctx;
    auto prefab = json::parse(R"({ "components": {
        "game::Hero": { "name": "Guard", "level": 5 },
        "game::Asleep": {},
        "game::NotARealComponent": { "x": 1 } } })");
    REQUIRE(prefab);
    Entity a = spawn_prefab(w, prefab.value, ctx);
    Entity b = spawn_prefab(w, prefab.value, ctx);
    CHECK(a != b);
    CHECK(w.get<Hero>(b).name == "Guard");
    CHECK(w.get<Hero>(b).level == 5);
    CHECK(w.has<Asleep>(a));
    bool warned = false;
    for (auto& msg : ctx.warnings) warned = warned || msg.find("NotARealComponent") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("world io: add hooks see loaded data") {
    register_components<Hero>();
    World w;
    std::string seen;
    w.on_add<Hero>([&](World& world, Entity e) { seen = world.get<Hero>(e).name; });
    serial::Context ctx;
    spawn_prefab(w, json::parse(R"({"components": {"game::Hero": {"name": "Loaded"}}})").value, ctx);
    CHECK(seen == "Loaded");
}

namespace {
// Records what an inspector would show.
struct Recorder final : reflect::FieldVisitor {
    std::vector<std::string> seen;
    void boolean(std::string_view n, bool&) override { seen.push_back(std::format("bool {}", n)); }
    void integer(std::string_view n, i64& v, i64, i64) override {
        seen.push_back(std::format("int {}", n));
        if (n == "level") v = 99; // edits write back
    }
    void real(std::string_view n, f64&) override { seen.push_back(std::format("real {}", n)); }
    void text(std::string_view n, std::string&) override { seen.push_back(std::format("text {}", n)); }
    void vector(std::string_view n, f32*, int c) override { seen.push_back(std::format("vec{} {}", c, n)); }
    void color(std::string_view n, Color&) override { seen.push_back(std::format("color {}", n)); }
    void entity(std::string_view n, u64) override { seen.push_back(std::format("entity {}", n)); }
    bool begin_group(std::string_view n) override {
        seen.push_back(std::format("group {}", n));
        return true;
    }
    void end_group() override { seen.push_back("end"); }
    void unsupported(std::string_view n, std::string_view) override { seen.push_back(std::format("? {}", n)); }
};
} // namespace

TEST_CASE("inspector visitor walks reflected components and writes edits back") {
    World w;
    Entity e = w.create(Hero{});
    const ComponentInfo& info = component_info(component_id<Hero>());
    REQUIRE(info.visit);
    Recorder r;
    info.visit(w.component_ptr(e, component_id<Hero>()), r);
    CHECK(r.seen == std::vector<std::string>{"text name", "int level", "bool alive", "int faction", "vec2 position",
                                             "color tint", "group stats", "int strength", "real speed", "end",
                                             "? inventory", "? buff", "entity companion"});
    CHECK(w.get<Hero>(e).level == 99);
}
