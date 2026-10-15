#include "ph/ecs/world_io.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <unordered_map>

namespace ph::ecs {

json::Value save_entities(const World& world, std::span<const Entity> entities, serial::Context& ctx) {
    // Pass 1: file ids for every saved entity, so references between them can be written.
    std::unordered_map<u64, u64> ids;
    std::vector<Entity> saved;
    for (Entity e : entities) {
        if (!world.alive(e) || ids.contains(e.bits())) continue;
        ids.emplace(e.bits(), saved.size());
        saved.push_back(e);
    }
    auto* previous = ctx.entity_map;
    ctx.entity_map = &ids;

    std::set<ComponentId> skipped;
    json::Value list;
    list.make_array();
    for (usize i = 0; i < saved.size(); ++i) {
        Entity e = saved[i];
        json::Value components;
        components.make_object();
        for (ComponentId c : world.components_of(e)) {
            const ComponentInfo& info = component_info(c);
            if (!info.savable()) {
                skipped.insert(c);
                continue;
            }
            components.set(info.name, info.save(world.component_ptr(e, c), ctx));
        }
        json::Value entry;
        entry.set("id", static_cast<f64>(i));
        entry.set("components", std::move(components));
        list.push(std::move(entry));
    }
    for (ComponentId c : skipped)
        ctx.warn(std::format("component '{}' isn't savable (add PH_REFLECT) and was skipped", component_info(c).name));
    ctx.entity_map = previous;

    json::Value doc;
    doc.set("format", world_format);
    doc.set("version", world_format_version);
    doc.set("entities", std::move(list));
    return doc;
}

json::Value save_world(const World& world, serial::Context& ctx, const QueryFilter& filter) {
    std::vector<Entity> entities;
    for (const auto& a : world.archetypes()) {
        if (a->empty() || !a->mask().contains(filter.with) || a->mask().intersects(filter.without)) continue;
        bool any_savable = false;
        for (ComponentId c : a->components()) any_savable = any_savable || component_info(c).savable();
        if (!any_savable) continue;
        entities.insert(entities.end(), a->entities().begin(), a->entities().end());
    }
    // Stable order (by entity index) so the same world always saves to the same file.
    std::sort(entities.begin(), entities.end(), [](Entity x, Entity y) { return x.index < y.index; });
    return save_entities(world, entities, ctx);
}

namespace {

void load_components(World& world, Entity e, const json::Value& components, serial::Context& ctx,
                     std::set<std::string>& unknown) {
    for (const auto& [name, data] : components.object()) {
        auto id = find_component(name);
        if (!id) {
            if (unknown.insert(name).second)
                ctx.warn(std::format("unknown component '{}' (register it with ecs::register_components<...>())", name));
            continue;
        }
        const ComponentInfo& info = component_info(*id);
        if (!info.savable()) {
            ctx.warn(std::format("component '{}' can't be loaded (not savable)", name));
            continue;
        }
        world.add_component(e, *id, [&](void* p) {
            if (!info.load(data, p, ctx)) ctx.warn(std::format("component '{}' loaded with errors", name));
        });
    }
}

} // namespace

std::vector<Entity> load_entities(World& world, const json::Value& doc, serial::Context& ctx) {
    std::vector<Entity> created;
    if (doc["format"].as_string() != world_format) {
        ctx.warn("not a saved world (missing \"format\")");
        return created;
    }
    if (doc["version"].as_int() > world_format_version) {
        ctx.warn(std::format("saved with a newer format version ({}); loading what's understood", doc["version"].as_int()));
    }
    // Pass 1: create every entity, so references can point forward as well as back.
    std::unordered_map<u64, u64> ids;
    for (const json::Value& entry : doc["entities"].array()) {
        Entity e = world.create();
        ids[static_cast<u64>(entry["id"].as_int(static_cast<i64>(created.size())))] = e.bits();
        created.push_back(e);
    }
    auto* previous = ctx.entity_map;
    ctx.entity_map = &ids;
    std::set<std::string> unknown;
    const json::Array& entries = doc["entities"].array();
    for (usize i = 0; i < entries.size(); ++i) load_components(world, created[i], entries[i]["components"], ctx, unknown);
    ctx.entity_map = previous;
    return created;
}

Entity spawn_prefab(World& world, const json::Value& entity, serial::Context& ctx) {
    Entity e = world.create();
    std::unordered_map<u64, u64> ids; // a prefab's own references to itself
    if (entity.contains("id")) ids[static_cast<u64>(entity["id"].as_int())] = e.bits();
    auto* previous = ctx.entity_map;
    ctx.entity_map = &ids;
    std::set<std::string> unknown;
    load_components(world, e, entity["components"], ctx, unknown);
    ctx.entity_map = previous;
    return e;
}

} // namespace ph::ecs
