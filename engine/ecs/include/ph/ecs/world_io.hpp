#pragma once

#include "ph/core/json.hpp"
#include "ph/core/serialize.hpp"
#include "ph/ecs/world.hpp"

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

} // namespace ph::ecs
