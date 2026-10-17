#include "bowl/ecs/archetype.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace bowl::ecs {

// Component registry

namespace {

struct Registry {
    std::array<ComponentInfo, max_components> infos;
    std::array<std::string, max_components> names; // owned: a registering module may be unloaded later
    // The type_name each was registered under; `names` may add " #2" or " (before reload)" to it.
    std::array<std::string, max_components> type_names;
    std::atomic<usize> count{0};
    std::mutex mutex;
};

Registry& registry() {
    static Registry r;
    return r;
}

// Whether two registrations under one name can be the same type seen from two modules: any difference rules it out.
bool same_shape(const ComponentInfo& a, const ComponentInfo& b) {
    return a.size == b.size && a.align == b.align && a.trivially_relocatable == b.trivially_relocatable &&
           !a.construct_default == !b.construct_default && !a.save == !b.save && !a.visit == !b.visit;
}

void relocate(const ComponentInfo& info, void* dst, void* src) {
    if (info.trivially_relocatable) {
        std::memcpy(dst, src, info.size);
    } else {
        info.move_construct(dst, src);
        info.destroy(src);
    }
}

} // namespace

ComponentId detail::register_component(const ComponentInfo& info) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    usize count = r.count.load(std::memory_order_relaxed);
    // One id per type name, whichever module asks: the host and a hot-reloaded game library share ids. The first
    // registration's code is kept; a dormant one (its library was unloaded) is refreshed by the next registration.
    // Some names don't identify one type, though: every file's anonymous namespace prints the same, and Clang leaves
    // the function out of a local type's name. So a registration under a known name is another type when it comes
    // from the same module with a different anchor (a type has one per module), has a local name, or has a different
    // shape; it gets an id of its own.
    bool local = bowl::detail::name_is_local(info.name);
    constexpr usize none = ~usize{0};
    usize revive = none, misfit = none;
    for (usize i = 0; i < count; ++i) {
        if (r.type_names[i] != info.name) continue;
        const ComponentInfo& old = r.infos[i];
        if (old.dormant) {
            // Plain data stays in place across a reload, so it has to keep fitting.
            bool may_hold_data = old.size > 0 && old.trivially_relocatable;
            bool fits =
                !may_hold_data || (old.size == info.size && old.align == info.align && info.trivially_relocatable);
            if (fits && revive == none) revive = i;
            if (!fits && misfit == none) misfit = i;
            continue;
        }
        if (old.type_key == info.type_key) return static_cast<ComponentId>(i);
        if (!local && old.module_key != info.module_key && same_shape(old, info)) return static_cast<ComponentId>(i);
    }
    if (revive != none) {
        r.infos[revive] = info;
        r.infos[revive].name = r.names[revive];
        return static_cast<ComponentId>(revive);
    }
    if (misfit != none) {
        // The type changed shape while entities still hold the old one: that data stays under a renamed type nothing
        // asks for, and the name gets a fresh id.
        r.names[misfit] += " (before reload)";
        r.type_names[misfit] = r.names[misfit];
        r.infos[misfit].name = r.names[misfit];
    }
    BOWL_ASSERT(count < max_components, "too many component types: raise BOWL_ECS_MAX_COMPONENTS");
    auto taken = [&](std::string_view name) {
        for (usize i = 0; i < count; ++i)
            if (r.names[i] == name) return true;
        return false;
    };
    std::string name(info.name);
    for (int n = 2; taken(name); ++n) name = std::format("{} #{}", info.name, n);
    r.names[count] = std::move(name);
    r.type_names[count] = std::string(info.name);
    r.infos[count] = info;
    r.infos[count].name = r.names[count];
    r.count.store(count + 1, std::memory_order_release);
    return static_cast<ComponentId>(count);
}

const ComponentInfo& component_info(ComponentId id) {
    BOWL_ASSERT(id < registry().count.load(std::memory_order_acquire));
    return registry().infos[id];
}

usize component_type_count() { return registry().count.load(std::memory_order_acquire); }

std::vector<std::pair<ComponentId, ComponentInfo>> mark_components_dormant(bool (*owned)(const ComponentInfo&, void*),
                                                                           void* user) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::vector<std::pair<ComponentId, ComponentInfo>> before;
    usize count = r.count.load(std::memory_order_relaxed);
    for (usize i = 0; i < count; ++i) {
        ComponentInfo& info = r.infos[i];
        if (info.dormant || !owned(info, user)) continue;
        before.emplace_back(static_cast<ComponentId>(i), info);
        // The code goes; the layout stays, since trivially copyable components keep their data in place (they're
        // copied and dropped without calling any of their code).
        ComponentInfo kept;
        kept.name = info.name;
        kept.size = info.size;
        kept.align = info.align;
        kept.trivially_relocatable = info.trivially_relocatable;
        kept.dormant = true;
        info = kept;
    }
    return before;
}

void restore_components(const std::vector<std::pair<ComponentId, ComponentInfo>>& saved) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (const auto& [id, info] : saved) r.infos[id] = info;
}

std::optional<ComponentId> find_component(std::string_view name) {
    usize n = component_type_count();
    for (usize i = 0; i < n; ++i)
        if (registry().infos[i].name == name) return static_cast<ComponentId>(i);
    return std::nullopt;
}

// Archetype

Archetype::Archetype(u32 id, std::vector<ComponentId> components) : id_(id), components_(std::move(components)) {
    std::sort(components_.begin(), components_.end());
    column_index_.fill(-1);
    columns_.reserve(components_.size());
    for (usize i = 0; i < components_.size(); ++i) {
        ComponentId c = components_[i];
        mask_.set(c);
        column_index_[c] = static_cast<i16>(i);
        columns_.push_back({&component_info(c), nullptr});
    }
}

Archetype::~Archetype() {
    for (u32 row = 0; row < entities_.size(); ++row) destroy_row(row);
    for (Column& col : columns_)
        if (col.data) ::operator delete(col.data, std::align_val_t(col.info->align));
}

void Archetype::reserve(usize capacity) {
    if (capacity <= capacity_) return;
    for (Column& col : columns_) {
        const ComponentInfo& info = *col.info;
        if (info.size == 0) continue;
        auto* fresh = static_cast<u8*>(::operator new(capacity * info.size, std::align_val_t(info.align)));
        if (col.data) {
            if (info.trivially_relocatable) {
                std::memcpy(fresh, col.data, entities_.size() * info.size);
            } else {
                for (usize i = 0; i < entities_.size(); ++i)
                    relocate(info, fresh + i * info.size, col.data + i * info.size);
            }
            ::operator delete(col.data, std::align_val_t(info.align));
        }
        col.data = fresh;
    }
    capacity_ = capacity;
}

u32 Archetype::push(Entity e) {
    if (entities_.size() == capacity_) reserve(std::max<usize>(16, capacity_ * 2));
    entities_.push_back(e);
    return static_cast<u32>(entities_.size() - 1);
}

void Archetype::destroy_row(u32 row) {
    for (Column& col : columns_)
        if (col.info->size && !col.info->trivially_relocatable) col.info->destroy(col.data + row * col.info->size);
}

Entity Archetype::remove(u32 row) {
    destroy_row(row);
    u32 last = static_cast<u32>(entities_.size() - 1);
    Entity moved = null_entity;
    if (row != last) {
        for (Column& col : columns_)
            if (col.info->size) relocate(*col.info, col.data + row * col.info->size, col.data + last * col.info->size);
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    return moved;
}

u32 Archetype::move_to(u32 row, Archetype& dst, Entity& moved) {
    u32 dst_row = dst.push(entities_[row]);
    for (usize i = 0; i < columns_.size(); ++i) {
        const ComponentInfo& info = *columns_[i].info;
        if (info.size == 0) continue;
        u8* src = columns_[i].data + row * info.size;
        if (void* to = dst.at(components_[i], dst_row))
            relocate(info, to, src);
        else if (!info.trivially_relocatable)
            info.destroy(src);
    }
    u32 last = static_cast<u32>(entities_.size() - 1);
    moved = null_entity;
    if (row != last) {
        for (Column& col : columns_)
            if (col.info->size) relocate(*col.info, col.data + row * col.info->size, col.data + last * col.info->size);
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    return dst_row;
}

} // namespace bowl::ecs
