#include "ph/ecs/archetype.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace ph::ecs {

// --- component registry ---------------------------------------------------------------------------------------------

namespace {

struct Registry {
    std::array<ComponentInfo, max_components> infos;
    std::atomic<usize> count{0};
    std::mutex mutex;
};

Registry& registry() {
    static Registry r;
    return r;
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
    usize id = r.count.load(std::memory_order_relaxed);
    PH_ASSERT(id < max_components, "too many component types: raise PH_ECS_MAX_COMPONENTS");
    r.infos[id] = info;
    r.count.store(id + 1, std::memory_order_release);
    return static_cast<ComponentId>(id);
}

const ComponentInfo& component_info(ComponentId id) {
    PH_ASSERT(id < registry().count.load(std::memory_order_acquire));
    return registry().infos[id];
}

usize component_type_count() { return registry().count.load(std::memory_order_acquire); }

std::optional<ComponentId> find_component(std::string_view name) {
    usize n = component_type_count();
    for (usize i = 0; i < n; ++i)
        if (registry().infos[i].name == name) return static_cast<ComponentId>(i);
    return std::nullopt;
}

// --- Archetype ------------------------------------------------------------------------------------------------------

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

} // namespace ph::ecs
