#include "ph/app/inspector.hpp"
#include "ph/core/file.hpp"
#include "ph/ecs/ecs.hpp"

#include <doctest/doctest.h>

#include <filesystem>

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

namespace game {
struct Loadout {
    std::vector<i32> items;
    std::array<f32, 3> weights{};
    std::vector<bool> flags;
};
PH_REFLECT(Loadout, items, weights, flags)
} // namespace game

TEST_CASE("inspector visitor walks lists and can resize vectors") {
    struct Lists final : reflect::FieldVisitor {
        std::vector<std::string> seen;
        void boolean(std::string_view n, bool& v) override {
            seen.push_back(std::format("bool {}", n));
            v = true;
        }
        void integer(std::string_view n, i64& v, i64, i64) override {
            seen.push_back(std::format("int {}", n));
            v = 7;
        }
        void real(std::string_view n, f64& v) override {
            seen.push_back(std::format("real {}", n));
            v = 0.5;
        }
        void text(std::string_view, std::string&) override {}
        void vector(std::string_view, f32*, int) override {}
        void color(std::string_view, Color&) override {}
        void entity(std::string_view, u64) override {}
        bool begin_group(std::string_view) override { return true; }
        void end_group() override {}
        void unsupported(std::string_view n, std::string_view) override { seen.push_back(std::format("? {}", n)); }
        bool begin_list(std::string_view n, usize& size, bool resizable) override {
            seen.push_back(std::format("list {} {} {}", n, size, resizable ? "resizable" : "fixed"));
            if (resizable) ++size; // the + button
            return true;
        }
        void end_list() override { seen.push_back("end list"); }
    };
    game::Loadout l{.items = {1}, .flags = {false}};
    Lists v;
    reflect::visit(v, "loadout", l);
    CHECK(l.items == std::vector<i32>{7, 7});
    CHECK(l.weights == std::array<f32, 3>{0.5f, 0.5f, 0.5f});
    CHECK(l.flags == std::vector<bool>{true, true});
    CHECK(v.seen.front() == "list items 1 resizable");
    CHECK(std::find(v.seen.begin(), v.seen.end(), "list weights 3 fixed") != v.seen.end());
    CHECK(std::find(v.seen.begin(), v.seen.end(), "int [1]") != v.seen.end());
}

TEST_CASE("inspector filter matches component names, Name and #index") {
    register_components<Hero, Asleep>();
    World w;
    Entity a = w.create(Hero{}, ecs::Name{"Ada the Brave"});
    Entity b = w.create(Hero{}, Asleep{});
    CHECK(inspector::matches(w, a, "hero"));
    CHECK(inspector::matches(w, a, "HERO brave"));
    CHECK_FALSE(inspector::matches(w, a, "asleep"));
    CHECK(inspector::matches(w, b, "asleep hero"));
    CHECK(inspector::matches(w, b, std::format("#{}", b.index)));
    CHECK_FALSE(inspector::matches(w, a, std::format("#{}", b.index)));
    CHECK(inspector::matches(w, a, ""));
}

TEST_CASE("components: hot-reload dormancy keeps ids and is undone by restore or re-registration") {
    ComponentId id = component_id<Hero>();
    auto everything = [](const ComponentInfo& info, void*) { return info.name == "game::Hero"; };
    auto saved = mark_components_dormant(everything, nullptr);
    REQUIRE(saved.size() == 1);
    CHECK(component_info(id).dormant);
    CHECK_FALSE(component_info(id).savable());
    restore_components(saved);
    CHECK_FALSE(component_info(id).dormant);
    CHECK(component_info(id).savable());

    mark_components_dormant(everything, nullptr);
    CHECK(ecs::detail::register_component(ecs::detail::make_component_info<Hero>()) == id); // same id, refreshed
    CHECK(component_info(id).savable());
    CHECK(find_component("game::Hero") == id);
}

namespace game {
struct Velocity2 { // plain data, not reflected: kept in place across a reload
    f32 x = 0, y = 0;
};
struct Velocity2Grown { // the same type after an edit that added a field
    f32 x = 0, y = 0, z = 0;
};
} // namespace game

TEST_CASE("components: plain data stays in place while dormant, and a changed layout is set aside") {
    World w;
    Entity e = w.create(game::Velocity2{3, 4});
    ComponentId id = component_id<game::Velocity2>();
    auto mine = [](const ComponentInfo& info, void*) { return info.name == "game::Velocity2"; };
    mark_components_dormant(mine, nullptr);
    CHECK(component_info(id).dormant);
    CHECK(component_info(id).size == sizeof(game::Velocity2)); // the layout survives the code
    // Moving the entity to another archetype copies the plain data without calling the (unloaded) code.
    w.add<Asleep>(e);
    CHECK(w.component_ptr(e, id) != nullptr);
    // Same layout comes back: same id, data untouched.
    CHECK(ecs::detail::register_component(ecs::detail::make_component_info<game::Velocity2>()) == id);
    CHECK(w.get<game::Velocity2>(e).y == 4);

    // A different layout under the same name gets a new id; the old data is parked under a renamed type.
    mark_components_dormant(mine, nullptr);
    auto grown = ecs::detail::make_component_info<game::Velocity2Grown>();
    grown.name = "game::Velocity2";
    ComponentId fresh = ecs::detail::register_component(grown);
    CHECK(fresh != id);
    CHECK(find_component("game::Velocity2") == fresh);
    CHECK(component_info(id).name == "game::Velocity2 (before reload)");
    w.destroy(e); // dropping the parked data calls no code
    CHECK(w.entity_count() == 0);
}

TEST_CASE("json: binary documents round-trip, are smaller, and refuse damage") {
    auto text = json::parse(R"({ "name": "Ada", "n": [0, 1, -1, 300, -70000, 1.5, 0.1, 1e300, 9007199254740993, -0.0],
        "flags": [true, false, null], "nested": { "name": "Ada", "deep": [[[]], {}] } })");
    REQUIRE(text);
    std::vector<u8> bin = json::encode_binary(text.value);
    CHECK(json::is_binary(bin));
    auto back = json::decode_binary(bin);
    REQUIRE(back);
    CHECK(back.value == text.value);
    CHECK(json::parse_any(bin).value == text.value);
    std::string plain = json::stringify(text.value, -1);
    CHECK(json::parse_any(std::span(reinterpret_cast<const u8*>(plain.data()), plain.size())).value == text.value);

    // A big world: keys and component names repeat, so binary is much smaller.
    register_components<Hero, Asleep>();
    World w;
    for (int i = 0; i < 2000; ++i) w.create(Hero{.name = "Guard", .level = i, .position = {f32(i), 2}});
    serial::Context ctx;
    json::Value world_doc = save_world(w, ctx);
    std::vector<u8> world_bin = json::encode_binary(world_doc);
    CHECK(world_bin.size() * 3 < json::stringify(world_doc, -1).size());
    CHECK(json::decode_binary(world_bin).value == world_doc);

    // Every truncation fails cleanly.
    for (usize n = 0; n < bin.size(); ++n) CHECK_FALSE(json::decode_binary(std::span(bin.data(), n)));
    std::vector<u8> bad = bin;
    bad.back() = 0x7F; // an unknown tag
    CHECK_FALSE(json::decode_binary(bad));
}

TEST_CASE("world io: migrations upgrade old saves step by step") {
    auto parsed = json::parse(R"({ "format": "Bowl.world", "version": 1, "entities": [
        { "id": 0, "components": { "game::OldHero": { "title": "Ada", "lvl": 4 }, "game::Removed": {} } } ] })");
    REQUIRE(parsed);
    json::Value doc = parsed.value;
    CHECK(game_version_of(doc) == 0);

    Migrations m;
    m.add(0, [](json::Value& d) { rename_component(d, "game::OldHero", "game::Hero"); });
    m.add(1, [](json::Value& d) {
        rename_field(d, "game::Hero", "title", "name");
        rename_field(d, "game::Hero", "lvl", "level");
        remove_component(d, "game::Removed");
    });
    m.add(2, [](json::Value& d) {
        for_each_saved(d, "game::Hero", [](json::Value& hero) { hero.set("level", hero["level"].as_int() * 10); });
    });
    CHECK(m.latest() == 3);
    serial::Context ctx;
    REQUIRE(m.upgrade(doc, ctx));
    CHECK(game_version_of(doc) == 3);
    CHECK(m.upgrade(doc, ctx)); // already current: nothing to do

    World w;
    auto loaded = load_entities(w, doc, ctx);
    REQUIRE(loaded.size() == 1);
    CHECK(w.get<Hero>(loaded[0]).name == "Ada");
    CHECK(w.get<Hero>(loaded[0]).level == 40);
    CHECK(ctx.warnings.empty());

    json::Value newer = doc;
    newer.set("game_version", 9);
    serial::Context c2;
    CHECK_FALSE(m.upgrade(newer, c2));
    CHECK(c2.warnings.size() == 1);

    Migrations gap;
    gap.add(0, [](json::Value&) {});
    gap.add(2, [](json::Value&) {});
    json::Value old = parsed.value;
    serial::Context c3;
    CHECK_FALSE(gap.upgrade(old, c3));
    CHECK(game_version_of(old) == 1); // got as far as it could

    serial::Context save_ctx{.game_version = 3};
    CHECK(game_version_of(save_world(w, save_ctx)) == 3);
}

TEST_CASE("world io: prefab files, one entity or several, text or binary") {
    register_components<Hero, Asleep>();
    auto dir = std::filesystem::temp_directory_path() / "ph_prefab_test";
    std::filesystem::create_directories(dir);
    std::string guard = R"({ "components": { "game::Hero": { "name": "Guard", "level": 5 }, "game::Asleep": {} } })";
    REQUIRE(write_file_bytes(dir / "guard.prefab", guard.data(), guard.size()));
    // Two entities that point at each other, saved as binary.
    World src;
    Entity a = src.create(Hero{.name = "Rider"});
    Entity b = src.create(Hero{.name = "Horse"});
    src.get<Hero>(a).companion = b;
    serial::Context ctx;
    std::vector<u8> pair = json::encode_binary(save_entities(src, std::vector{a, b}, ctx));
    REQUIRE(write_file_bytes(dir / "pair.prefab", pair.data(), pair.size()));

    AssetManager assets;
    assets.mount("", dir);
    register_world_file_loader(assets);
    auto one = assets.load<Prefab>("guard.prefab");
    auto two = assets.load<Prefab>("pair.prefab");
    REQUIRE(one);
    REQUIRE(two);
    World w;
    auto g1 = instantiate(w, *one, ctx), g2 = instantiate(w, *one, ctx);
    REQUIRE(g1.size() == 1);
    CHECK(g1[0] != g2[0]);
    CHECK(w.get<Hero>(g2[0]).level == 5);
    CHECK(w.has<Asleep>(g1[0]));
    auto riders = instantiate(w, *two, ctx);
    REQUIRE(riders.size() == 2);
    CHECK(w.get<Hero>(riders[0]).companion == riders[1]);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
