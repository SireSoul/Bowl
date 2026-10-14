#pragma once

#include "ph/core/assert.hpp"
#include "ph/core/reflect.hpp"
#include "ph/core/serialize.hpp"
#include "ph/core/type_id.hpp"
#include "ph/core/types.hpp"

#include <array>
#include <bit>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef PH_ECS_MAX_COMPONENTS
#define PH_ECS_MAX_COMPONENTS 256
#endif

namespace ph::ecs {

// An entity is an index plus a generation. Destroying an entity bumps its slot's generation, so stale handles are
// detected instead of silently aliasing a new entity. Generation 0 is never used: Entity{} is the null entity.
struct Entity {
    u32 index = 0;
    u32 generation = 0;

    constexpr bool is_null() const { return generation == 0; }
    constexpr explicit operator bool() const { return generation != 0; }
    constexpr u64 bits() const { return (static_cast<u64>(generation) << 32) | index; }
    static constexpr Entity from_bits(u64 b) { return {static_cast<u32>(b), static_cast<u32>(b >> 32)}; }
    constexpr bool operator==(const Entity&) const = default;
};

inline constexpr Entity null_entity{};

// Entities in saved data are written as ids local to the file and remapped on load (ctx.entity_map), so references
// between saved entities survive. A reference to an entity that wasn't saved becomes null.
inline json::Value ph_to_json(const Entity& e, serial::Context& ctx) {
    if (!e) return {};
    if (ctx.entity_map) {
        auto it = ctx.entity_map->find(e.bits());
        if (it != ctx.entity_map->end()) return static_cast<f64>(it->second);
    }
    ctx.warn("a reference to an entity that isn't being saved was written as null");
    return {};
}
inline bool ph_from_json(const json::Value& j, Entity& e, serial::Context& ctx) {
    if (j.is_null()) {
        e = null_entity;
        return true;
    }
    if (!j.is_number()) return false;
    e = null_entity;
    if (ctx.entity_map) {
        auto it = ctx.entity_map->find(static_cast<u64>(j.as_int()));
        if (it != ctx.entity_map->end()) e = Entity::from_bits(it->second);
    }
    return true;
}
inline void ph_visit(reflect::FieldVisitor& v, std::string_view name, Entity& e) { v.entity(name, e.bits()); }

using ComponentId = u16;
inline constexpr usize max_components = PH_ECS_MAX_COMPONENTS;
static_assert(max_components % 64 == 0);

// Components are plain structs: any type that is move-constructible and destructible. No base class, no macros,
// no code generation. Empty structs are "tags" and take no memory.
template <class T>
concept Component = std::is_object_v<T> && !std::is_const_v<T> && std::is_move_constructible_v<T> &&
                    std::is_destructible_v<T>;

struct ComponentInfo {
    std::string_view name;
    u32 size = 0; // 0 for tags
    u32 align = 1;
    bool trivially_relocatable = false;
    void (*move_construct)(void* dst, void* src) = nullptr; // placement-move src into uninitialized dst
    void (*destroy)(void* ptr) = nullptr;
    // Present for savable components: reflected (PH_REFLECT), with serialization hooks, or tags.
    void (*construct_default)(void* ptr) = nullptr;
    json::Value (*save)(const void* ptr, serial::Context& ctx) = nullptr;
    bool (*load)(const json::Value& j, void* ptr, serial::Context& ctx) = nullptr;
    void (*visit)(void* ptr, reflect::FieldVisitor& v) = nullptr; // live editing (reflected only)
    // Its code was in a game library that hot reload unloaded; the next registration of the name refreshes it.
    bool dormant = false;
    bool savable() const { return save != nullptr && !dormant; }
};

template <class T>
concept SavableComponent = std::is_default_constructible_v<T> &&
                           (std::is_empty_v<T> || reflect::Reflected<T> || serial::HasHooks<T>);

namespace detail {
ComponentId register_component(const ComponentInfo& info);

template <Component T> ComponentInfo make_component_info() {
    ComponentInfo info;
    info.name = type_name<T>();
    if constexpr (!std::is_empty_v<T>) {
        info.size = sizeof(T);
        info.align = alignof(T);
        info.trivially_relocatable = std::is_trivially_copyable_v<T>;
        info.move_construct = [](void* dst, void* src) { ::new (dst) T(std::move(*static_cast<T*>(src))); };
        info.destroy = [](void* p) { static_cast<T*>(p)->~T(); };
    }
    if constexpr (SavableComponent<T>) {
        info.construct_default = [](void* p) {
            if constexpr (!std::is_empty_v<T>) ::new (p) T();
        };
        if constexpr (std::is_empty_v<T>) {
            info.save = [](const void*, serial::Context&) { return json::Value(json::Object{}); };
            info.load = [](const json::Value&, void*, serial::Context&) { return true; };
        } else {
            info.save = [](const void* p, serial::Context& ctx) { return serial::to_json(*static_cast<const T*>(p), ctx); };
            info.load = [](const json::Value& j, void* p, serial::Context& ctx) {
                return serial::from_json(j, *static_cast<T*>(p), ctx);
            };
        }
        if constexpr (reflect::Reflected<T>) {
            info.visit = [](void* p, reflect::FieldVisitor& v) {
                reflect::for_each_field(*static_cast<T*>(p),
                                        [&](std::string_view name, auto& field) { reflect::visit(v, name, field); });
            };
        }
    }
    return info;
}
} // namespace detail

const ComponentInfo& component_info(ComponentId id);
usize component_type_count();
// Looks a component type up by its type_name (e.g. "game::Health"). Only types used (or registered) so far exist.
std::optional<ComponentId> find_component(std::string_view name);

namespace detail {
template <class T> ComponentId component_id_of() {
    static_assert(Component<T>, "components must be move-constructible, destructible object types");
    static const ComponentId id = register_component(make_component_info<T>());
    return id;
}
} // namespace detail

// const T and T share one id (const only expresses read-only access in queries).
template <class T> ComponentId component_id() { return detail::component_id_of<std::remove_cvref_t<T>>(); }

// Makes component types known by name before anything uses them, so saved worlds that mention them can load.
template <class... Ts> void register_components() { (static_cast<void>(component_id<Ts>()), ...); }

// Hot reload: marks registrations whose code satisfies `owned` (lives in the library being unloaded) as dormant, and
// returns what they were so restore_components can undo it if the new library fails to load.
std::vector<std::pair<ComponentId, ComponentInfo>> mark_components_dormant(bool (*owned)(const ComponentInfo&, void*),
                                                                           void* user);
void restore_components(const std::vector<std::pair<ComponentId, ComponentInfo>>& saved);

} // namespace ph::ecs

// PH_COMPONENT(Type, fields...): PH_REFLECT plus registration when the program (or game library) loads, so saved
// worlds can name the type before any code touches it. Use it for components; PH_REFLECT for other structs.
#define PH_COMPONENT(Type, ...)                                                                                      \
    PH_REFLECT(Type, __VA_ARGS__)                                                                                     \
    [[maybe_unused]] inline const ::ph::ecs::ComponentId ph_component_registration_##Type =                           \
        ::ph::ecs::component_id<Type>();

namespace ph::ecs {

// Fixed-size bitset over component ids.
struct ComponentMask {
    std::array<u64, max_components / 64> words{};

    void set(ComponentId id) { words[id >> 6] |= u64{1} << (id & 63); }
    void reset(ComponentId id) { words[id >> 6] &= ~(u64{1} << (id & 63)); }
    bool test(ComponentId id) const { return (words[id >> 6] >> (id & 63)) & 1; }
    bool contains(const ComponentMask& o) const {
        for (usize i = 0; i < words.size(); ++i)
            if ((words[i] & o.words[i]) != o.words[i]) return false;
        return true;
    }
    bool intersects(const ComponentMask& o) const {
        for (usize i = 0; i < words.size(); ++i)
            if (words[i] & o.words[i]) return true;
        return false;
    }
    bool empty() const {
        for (u64 w : words)
            if (w) return false;
        return true;
    }
    ComponentMask operator|(const ComponentMask& o) const {
        ComponentMask m;
        for (usize i = 0; i < words.size(); ++i) m.words[i] = words[i] | o.words[i];
        return m;
    }
    bool operator==(const ComponentMask&) const = default;
    usize hash() const {
        u64 h = 0xcbf29ce484222325ull;
        for (u64 w : words) h = (h ^ w) * 0x100000001b3ull;
        return static_cast<usize>(h);
    }
    template <class F> void for_each(F&& f) const {
        for (usize i = 0; i < words.size(); ++i)
            for (u64 w = words[i]; w; w &= w - 1) f(static_cast<ComponentId>(i * 64 + static_cast<usize>(std::countr_zero(w))));
    }

    template <class... Ts> static ComponentMask of() {
        ComponentMask m;
        (m.set(component_id<Ts>()), ...);
        return m;
    }
};

struct ComponentMaskHash {
    usize operator()(const ComponentMask& m) const { return m.hash(); }
};

} // namespace ph::ecs

template <> struct std::hash<ph::ecs::Entity> {
    std::size_t operator()(const ph::ecs::Entity& e) const noexcept { return std::hash<ph::u64>{}(e.bits()); }
};
