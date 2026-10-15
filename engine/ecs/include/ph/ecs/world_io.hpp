#pragma once

#include "ph/core/json.hpp"
#include "ph/core/serialize.hpp"
#include "ph/ecs/world.hpp"

#include <functional>
#include <map>
#include <span>
#include <vector>

namespace ph::ecs {

// Saving and loading entities as JSON, for save games, scenes, prefabs and copy/paste in tools.
//
//     { "format": "Bowl.world", "version": 1,
//       "entities": [ { "id": 0, "components": { "game::Health": { "current": 80, "max": 100 }, "game::Player": {} } } ] }
//
// Components are saved by type name when they're savable: reflected with PH_REFLECT, given ph_to_json/ph_from_json
// hooks, or empty tags. Others are skipped (once-per-type warning in ctx.warnings). Entity fields that point at other
// saved entities are rewritten to file ids and back, so references survive a round trip.
//
// Documents can be written as text (json::stringify) or, for big worlds, binary (json::encode_binary); everything that
// reads world files takes either.

inline constexpr std::string_view world_format = "Bowl.world";
inline constexpr int world_format_version = 1;

// Saves the given entities (dead ones are skipped).
json::Value save_entities(const World& world, std::span<const Entity> entities, serial::Context& ctx);
// Saves every entity that has at least one savable component and passes the filter.
json::Value save_world(const World& world, serial::Context& ctx, const QueryFilter& filter = {});

// Spawns the saved entities into the world (adding to what's there) and returns them in file order.
// Unknown component names are skipped with a warning. Types the program hasn't used yet are unknown until
// registered: use PH_COMPONENT, or call register_components<...>() before loading.
std::vector<Entity> load_entities(World& world, const json::Value& doc, serial::Context& ctx);

// A prefab is one saved entity object ({"components": {...}}); spawns a fresh copy.
Entity spawn_prefab(World& world, const json::Value& entity, serial::Context& ctx);

// A saved world file (".world", what the editor writes) as an asset. It hot-reloads like any file; spawn its contents
// with load_entities(world, file->document, ctx).
struct WorldFile {
    json::Value document;
};

// A prefab file (".prefab"): one entity ({"components": {...}}), or several as a world document whose entities refer to
// each other (a cart and its wheels). Hot-reloads; copies already spawned stay as they are.
struct Prefab {
    json::Value document; // always a world document
};
// Spawns a copy of every entity in the prefab; the first is its root.
std::vector<Entity> instantiate(World& world, const Prefab& prefab, serial::Context& ctx);

// Registers the WorldFile and Prefab loaders. Both read text or binary files.
void register_world_file_loader(AssetManager& assets);

// Upgrades saves written by older versions of a game. Number your save format in serial::Context::game_version (it's
// written into every world file) and add one step for each change, before loading:
//
//     ecs::Migrations migrations;
//     migrations.add(1, [](json::Value& doc) { ecs::rename_component(doc, "game::Hp", "game::Health"); });  // 1 -> 2
//     migrations.add(2, [](json::Value& doc) { ecs::rename_field(doc, "game::Health", "hp", "current"); }); // 2 -> 3
//     migrations.upgrade(doc, ctx);
//     ecs::load_entities(world, doc, ctx);
//
// Files written before a game had a version count as version 0.
class Migrations {
public:
    using Step = std::function<void(json::Value& document)>;

    // A step that turns a document at `from_version` into from_version + 1.
    void add(i32 from_version, Step step);
    // The version the last step produces (0 with no steps).
    i32 latest() const;
    // Runs the steps from the document's version up to latest() and records the new version. False, with a warning,
    // when a step is missing or the file comes from a newer game than this one.
    bool upgrade(json::Value& document, serial::Context& ctx) const;

private:
    std::map<i32, Step> steps_;
};

// The document's "game_version" (0 when missing).
i32 game_version_of(const json::Value& document);

// Helpers for migration steps; each works on every saved entity in a world document.
void rename_component(json::Value& document, std::string_view from, std::string_view to);
void remove_component(json::Value& document, std::string_view name);
void rename_field(json::Value& document, std::string_view component, std::string_view from, std::string_view to);
// Calls `change` with each saved copy of a component, to rework its data.
void for_each_saved(json::Value& document, std::string_view component, const std::function<void(json::Value&)>& change);

} // namespace ph::ecs
