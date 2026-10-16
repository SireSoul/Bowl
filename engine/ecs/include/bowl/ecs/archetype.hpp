#pragma once

#include "bowl/ecs/component.hpp"

#include <span>
#include <unordered_map>
#include <vector>

namespace bowl::ecs {

// All entities with exactly the same set of components live in one archetype, each component type in its own tightly
// packed array (structure of arrays), so a query walks memory front to back.
class Archetype {
public:
    Archetype(u32 id, std::vector<ComponentId> components);
    ~Archetype();
    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;

    u32 id() const { return id_; }
    const ComponentMask& mask() const { return mask_; }
    std::span<const ComponentId> components() const { return components_; }
    usize size() const { return entities_.size(); }
    bool empty() const { return entities_.empty(); }
    std::span<const Entity> entities() const { return entities_; }
    bool has(ComponentId c) const { return mask_.test(c); }

    // Base pointer of a component column (nullptr for tags, or if the archetype lacks the component).
    void* column(ComponentId c) const {
        i16 i = column_index_[c];
        return i < 0 ? nullptr : columns_[static_cast<usize>(i)].data;
    }
    void* at(ComponentId c, u32 row) const {
        i16 i = column_index_[c];
        if (i < 0) return nullptr;
        const Column& col = columns_[static_cast<usize>(i)];
        return col.data ? col.data + static_cast<usize>(row) * col.info->size : nullptr;
    }

    // The rest is for World.

    // Appends a row whose components are uninitialized; the caller constructs them.
    u32 push(Entity e);
    // Destroys the row's components and fills the hole with the last row. Returns the entity that moved into `row`
    // (null if `row` was the last row).
    Entity remove(u32 row);
    // Moves the row to `dst`, carrying over the components both archetypes share. Components only `dst` has are left
    // uninitialized for the caller. Returns the new row in `dst`; `moved` receives the entity that took the old row.
    u32 move_to(u32 row, Archetype& dst, Entity& moved);

    std::unordered_map<ComponentId, Archetype*> add_edges;
    std::unordered_map<ComponentId, Archetype*> remove_edges;

private:
    struct Column {
        const ComponentInfo* info;
        u8* data = nullptr;
    };
    void reserve(usize capacity);
    void destroy_row(u32 row);

    u32 id_;
    ComponentMask mask_;
    std::vector<ComponentId> components_;
    std::vector<Column> columns_;
    std::array<i16, max_components> column_index_;
    std::vector<Entity> entities_;
    usize capacity_ = 0;
};

} // namespace bowl::ecs
