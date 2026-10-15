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
    if (ctx.game_version > 0) doc.set("game_version", ctx.game_version);
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

std::vector<Entity> instantiate(World& world, const Prefab& prefab, serial::Context& ctx) {
    return load_entities(world, prefab.document, ctx);
}

namespace {

// Reads a text or binary document for a loader; null (with the error logged) if it can't.
std::optional<json::Value> read_document(AssetLoadContext& ctx, std::string_view type) {
    auto bytes = ctx.read(ctx.path());
    if (!bytes) {
        log_asset_error(ctx.path(), type, "file not found");
        return std::nullopt;
    }
    auto parsed = json::parse_any(*bytes);
    if (!parsed) {
        log_asset_error(ctx.path(), type,
                        json::is_binary(*bytes)
                            ? std::format("byte {}: {}", parsed.error->column, parsed.error->message)
                            : std::format("{}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message));
        return std::nullopt;
    }
    return std::move(parsed.value);
}

} // namespace

void register_world_file_loader(AssetManager& assets) {
    assets.register_loader<WorldFile>([](AssetLoadContext& ctx) -> std::unique_ptr<WorldFile> {
        auto doc = read_document(ctx, "WorldFile");
        if (!doc) return nullptr;
        if ((*doc)["format"].as_string() != world_format) {
            log_asset_error(ctx.path(), "WorldFile", "not a saved world (missing \"format\")");
            return nullptr;
        }
        return std::make_unique<WorldFile>(WorldFile{std::move(*doc)});
    });
    assets.register_loader<Prefab>([](AssetLoadContext& ctx) -> std::unique_ptr<Prefab> {
        auto doc = read_document(ctx, "Prefab");
        if (!doc) return nullptr;
        if ((*doc)["format"].as_string() == world_format) return std::make_unique<Prefab>(Prefab{std::move(*doc)});
        if (!doc->contains("components")) {
            log_asset_error(ctx.path(), "Prefab", "expected {\"components\": {...}} or a saved world");
            return nullptr;
        }
        // One entity: wrap it in a world document.
        json::Value world_doc;
        world_doc.set("format", world_format);
        world_doc.set("version", world_format_version);
        json::Value entities;
        entities.push(std::move(*doc));
        world_doc.set("entities", std::move(entities));
        return std::make_unique<Prefab>(Prefab{std::move(world_doc)});
    });
}

// Migrations

void Migrations::add(i32 from_version, Step step) { steps_[from_version] = std::move(step); }

i32 Migrations::latest() const { return steps_.empty() ? 0 : steps_.rbegin()->first + 1; }

i32 game_version_of(const json::Value& document) { return static_cast<i32>(document["game_version"].as_int(0)); }

bool Migrations::upgrade(json::Value& document, serial::Context& ctx) const {
    i32 version = game_version_of(document);
    i32 target = latest();
    if (version > target) {
        ctx.warn(std::format("saved by a newer version of the game (save version {}, this game knows up to {})", version,
                             target));
        return false;
    }
    for (; version < target; ++version) {
        auto step = steps_.find(version);
        if (step == steps_.end()) {
            ctx.warn(std::format("no migration from save version {}", version));
            return false;
        }
        step->second(document);
        document.set("game_version", version + 1);
    }
    return true;
}

void for_each_saved(json::Value& document, std::string_view component, const std::function<void(json::Value&)>& change) {
    json::Value* entities = document.find("entities");
    if (!entities || !entities->is_array()) return;
    for (json::Value& entity : entities->make_array())
        if (json::Value* components = entity.find("components"))
            if (json::Value* c = components->find(component)) change(*c);
}

void rename_component(json::Value& document, std::string_view from, std::string_view to) {
    json::Value* entities = document.find("entities");
    if (!entities || !entities->is_array()) return;
    for (json::Value& entity : entities->make_array()) {
        json::Value* components = entity.find("components");
        if (!components || !components->is_object()) continue;
        for (auto& [name, data] : components->make_object())
            if (name == from) name = std::string(to); // in place, so the order in the file stays the same
    }
}

void remove_component(json::Value& document, std::string_view name) {
    json::Value* entities = document.find("entities");
    if (!entities || !entities->is_array()) return;
    for (json::Value& entity : entities->make_array())
        if (json::Value* components = entity.find("components")) components->erase(name);
}

void rename_field(json::Value& document, std::string_view component, std::string_view from, std::string_view to) {
    for_each_saved(document, component, [&](json::Value& data) {
        if (!data.is_object()) return;
        for (auto& [name, value] : data.make_object())
            if (name == from) name = std::string(to);
    });
}

} // namespace ph::ecs
