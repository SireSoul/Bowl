// Saved worlds and prefabs (text or binary JSON), as save games load them: migrations, then entities with their
// components, references between them, and saving again.

#include "fuzz.hpp"

#include "bowl/ecs/ecs.hpp"

using namespace bowl;
using namespace bowl::ecs;

namespace game {

enum class Faction : u8 { Wild, Town, Guild };

struct Stats {
    i32 strength = 1;
    f32 speed = 2.5f;
    u8 luck = 0;
    u16 gold = 0;
};
BOWL_REFLECT(Stats, strength, speed, luck, gold)

// A field of every kind saves can hold.
struct Hero {
    std::string name = "nobody";
    i32 level = 1;
    bool alive = true;
    Faction faction = Faction::Wild;
    Vec2 position;
    Vec3 facing;
    Color tint = Color::white();
    Rect bounds;
    Stats stats;
    std::vector<i32> inventory;
    std::array<u32, 3> slots{};
    std::optional<f32> buff;
    Entity companion;
    std::vector<Entity> party;
    u64 seed = 0;
    f64 played = 0;
    json::Value extra;
};
BOWL_REFLECT(Hero, name, level, alive, faction, position, facing, tint, bounds, stats, inventory, slots, buff,
             companion, party, seed, played, extra)

struct Asleep {};

} // namespace game

using namespace game;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool registered = (register_components<Hero, Asleep, Stats>(), true);
    (void)registered;
    auto parsed = json::parse_any({data, size});
    if (!parsed) return 0;
    json::Value doc = std::move(parsed.value);

    serial::Context ctx;
    Migrations migrations;
    migrations.add(0, [](json::Value& d) { rename_component(d, "game::OldHero", "game::Hero"); });
    migrations.add(1, [](json::Value& d) {
        rename_field(d, "game::Hero", "lvl", "level");
        remove_component(d, "game::Removed");
        for_each_saved(d, "game::Hero", [](json::Value& hero) { hero.set("level", hero["level"].as_number() + 1); });
    });
    migrations.upgrade(doc, ctx);

    World world;
    std::vector<Entity> loaded = load_entities(world, doc, ctx);
    spawn_prefab(world, doc, ctx);
    if (doc["entities"].size()) spawn_prefab(world, doc["entities"][0], ctx);

    // Saving what was loaded gives a document that loads again.
    json::Value saved = save_entities(world, loaded, ctx);
    World again;
    BOWL_FUZZ_CHECK(load_entities(again, json::decode_binary(json::encode_binary(saved)).value, ctx).size() ==
                    saved["entities"].size());
    return 0;
}
