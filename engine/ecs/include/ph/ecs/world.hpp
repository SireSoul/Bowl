#pragma once

#include "ph/core/jobs.hpp"
#include "ph/ecs/archetype.hpp"
#include "ph/ecs/commands.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ph::ecs {

template <class... Ts> class Query;

// Extra include/exclude terms for a query, for components you filter on but don't read.
struct QueryFilter {
    ComponentMask with, without;
    template <class... Ts> QueryFilter& require() { with = with | ComponentMask::of<Ts...>(); return *this; }
    template <class... Ts> QueryFilter& exclude() { without = without | ComponentMask::of<Ts...>(); return *this; }
};
template <class... Ts> QueryFilter with() { return QueryFilter{}.require<Ts...>(); }
template <class... Ts> QueryFilter without() { return QueryFilter{}.exclude<Ts...>(); }

using Hook = std::function<void(World&, Entity)>;

// Owns entities, their components, and singleton resources.
//
//     World world;
//     Entity e = world.create(Position{0, 0}, Velocity{1, 0});
//     world.each<Position, const Velocity>([](Position& p, const Velocity& v) { p.value += v.value; });
//
// Structural changes (create/destroy/add/remove) are not allowed while a query is iterating; record them in a
// CommandBuffer (world.deferred(), or the one systems receive) instead. Reading and writing component values of
// other entities with get()/try_get() is always fine.
class World : NonCopyable {
public:
    World();
    ~World();

    // --- entities --------------------------------------------------------------------------------------------------
    Entity create();
    template <class... Ts>
        requires(sizeof...(Ts) > 0 && (Component<std::decay_t<Ts>> && ...))
    Entity create(Ts&&... components) {
        Entity e = create();
        insert(e, std::forward<Ts>(components)...);
        return e;
    }
    void destroy(Entity e);
    bool alive(Entity e) const {
        return e.index < records_.size() && records_[e.index].alive && records_[e.index].generation == e.generation;
    }
    usize entity_count() const { return entity_count_; }

    // --- components ------------------------------------------------------------------------------------------------
    // Adds T (constructed from args) or replaces the existing one. Returns the stored component.
    template <Component T, class... Args> T& add(Entity e, Args&&... args);
    // Adds or replaces several components with a single archetype move.
    template <class... Ts>
        requires(Component<std::decay_t<Ts>> && ...)
    void insert(Entity e, Ts&&... components);
    template <Component T> void remove(Entity e);
    template <class T> bool has(Entity e) const {
        return alive(e) && records_[e.index].archetype->has(component_id<T>());
    }
    template <class T> T* try_get(Entity e) const;
    template <class T> T& get(Entity e) const {
        T* p = try_get<T>(e);
        PH_ASSERT(p, "entity is dead or lacks the component");
        return *p;
    }
    // Component ids of an entity, for tools (see component_info(id).name).
    std::span<const ComponentId> components_of(Entity e) const;
    void* component_ptr(Entity e, ComponentId c) const;
    // Adds a component by id (default-constructed, then `init` fills it in) — for loaders and tools that only know
    // the type at runtime. Replaces an existing one. The component type must be savable (see SavableComponent).
    // init receives nullptr for tags. Returns false if the type can't be default-constructed this way.
    bool add_component(Entity e, ComponentId c, FunctionRef<void(void*)> init);

    // --- queries ---------------------------------------------------------------------------------------------------
    // Ts may be const-qualified to declare read-only access. Keep the Query around (e.g. in a system) — it caches the
    // matching archetypes and updates incrementally.
    template <class... Ts> Query<Ts...> query(QueryFilter filter = {});
    // One-shot convenience: f(Ts&...) or f(Entity, Ts&...).
    template <class... Ts, class F> void each(F&& f);

    // --- hooks -----------------------------------------------------------------------------------------------------
    // on_add runs after T is added to an entity; on_remove runs before T is removed or its entity destroyed (the
    // component is still readable). Hooks run with the world locked; use deferred() for structural changes.
    template <Component T> void on_add(Hook hook) { hooks_for(component_id<T>()).on_add.push_back(std::move(hook)); }
    template <Component T> void on_remove(Hook hook) { hooks_for(component_id<T>()).on_remove.push_back(std::move(hook)); }

    // --- resources (singletons: time, input, renderer handles, game state, ...) ------------------------------------
    template <class T, class... Args> T& set_resource(Args&&... args);
    template <class T> T* try_resource() const;
    template <class T> T& resource() const {
        T* r = try_resource<T>();
        PH_ASSERT(r, "missing resource");
        return *r;
    }
    template <class T> void remove_resource() { resources_.erase(type_id<T>()); }

    // --- deferral --------------------------------------------------------------------------------------------------
    CommandBuffer& deferred() { return deferred_; }
    // Applies deferred() commands. The App calls this between schedules.
    void flush();
    bool locked() const { return lock_.load(std::memory_order_relaxed) > 0; }

    // --- introspection ---------------------------------------------------------------------------------------------
    std::span<const std::unique_ptr<Archetype>> archetypes() const { return archetypes_; }
    // Bumped whenever a new archetype appears; queries use it to refresh their caches.
    usize archetype_generation() const { return archetypes_.size(); }

    struct LockGuard {
        World& w;
        explicit LockGuard(World& world) : w(world) { w.lock_.fetch_add(1, std::memory_order_relaxed); }
        ~LockGuard() { w.lock_.fetch_sub(1, std::memory_order_relaxed); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };

private:
    friend class CommandBuffer;
    template <class... Ts> friend class Query;

    struct Record {
        Archetype* archetype = nullptr;
        u32 row = 0;
        u32 generation = 1;
        bool alive = false;
    };
    struct Hooks {
        std::vector<Hook> on_add, on_remove;
    };
    struct ResourceBase {
        virtual ~ResourceBase() = default;
    };
    template <class T> struct ResourceHolder final : ResourceBase {
        template <class... Args> explicit ResourceHolder(Args&&... args) : value(std::forward<Args>(args)...) {}
        T value;
    };

    // Entity id reservation, thread-safe while the world is locked (see CommandBuffer::create).
    Entity reserve_entity();
    void materialize_reserved();
    void spawn_reserved(Entity e);

    Archetype& archetype_for(const ComponentMask& mask);
    Archetype& archetype_with(Archetype& from, ComponentId added);
    Archetype& archetype_without(Archetype& from, ComponentId removed);
    // Moves e into `to`; returns its new row. Components `to` adds are uninitialized.
    u32 move_entity(Entity e, Archetype& to);
    void check_unlocked() const { PH_ASSERT(!locked(), "structural change while iterating: use a CommandBuffer"); }

    Hooks& hooks_for(ComponentId c);
    const Hooks* hooks_of(ComponentId c) const { return c < hooks_.size() && hooks_[c] ? hooks_[c].get() : nullptr; }
    void run_add_hooks(ComponentId c, Entity e);
    void run_remove_hooks(ComponentId c, Entity e);

    std::vector<Record> records_;
    std::vector<u32> free_;
    std::atomic<i64> free_cursor_{0};
    std::atomic<u32> pending_new_{0};
    usize entity_count_ = 0;

    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<ComponentMask, Archetype*, ComponentMaskHash> by_mask_;
    Archetype* empty_ = nullptr;

    std::vector<std::unique_ptr<Hooks>> hooks_;
    std::unordered_map<TypeId, std::unique_ptr<ResourceBase>> resources_;
    std::atomic<u32> lock_{0};
    CommandBuffer deferred_;
};

// --- Query ----------------------------------------------------------------------------------------------------------

namespace detail {

template <class T> struct Column {
    using U = std::remove_const_t<T>;
    T* base;
    explicit Column(const Archetype& a) {
        if constexpr (std::is_empty_v<U>) {
            static U tag{};
            base = &tag;
        } else {
            base = static_cast<U*>(a.column(component_id<U>()));
        }
    }
    T& operator[](usize i) const {
        if constexpr (std::is_empty_v<U>) return *base;
        else return base[i];
    }
};

template <class F, class... Ts> constexpr bool takes_entity = std::is_invocable_v<F&, Entity, Ts&...>;

template <class F, class... Ts> inline void call(F& f, Entity e, Ts&... cs) {
    if constexpr (takes_entity<F, Ts...>) f(e, cs...);
    else f(cs...);
}

} // namespace detail

template <class... Ts> class Query {
    static_assert(sizeof...(Ts) > 0, "a query needs at least one component");

public:
    explicit Query(World& world, QueryFilter filter = {}) : world_(&world) {
        include_ = ComponentMask::of<std::remove_const_t<Ts>...>() | filter.with;
        exclude_ = filter.without;
    }

    // f(Ts&...) or f(Entity, Ts&...).
    template <class F> void each(F&& f) {
        refresh();
        World::LockGuard lock(*world_);
        for (Archetype* a : matches_) {
            usize n = a->size();
            if (n == 0) continue;
            std::tuple<detail::Column<Ts>...> cols{detail::Column<Ts>(*a)...};
            const Entity* entities = a->entities().data();
            for (usize i = 0; i < n; ++i)
                std::apply([&](auto&... c) { detail::call<F, Ts...>(f, entities[i], c[i]...); }, cols);
        }
    }

    // Like each, split across worker threads. f must only touch its own entity's components (plus read-only data).
    template <class F> void par_each(JobSystem& jobs, F&& f, usize min_chunk = 256) {
        par_each_chunked(jobs, min_chunk, [&](usize, Entity e, Ts&... cs) { detail::call<F, Ts...>(f, e, cs...); });
    }

    // f(chunk_index, Entity, Ts&...). Chunks depend only on the entity count and min_chunk, so per-chunk outputs
    // (e.g. command buffers) can be merged in chunk order for deterministic results.
    template <class F> void par_each_chunked(JobSystem& jobs, usize min_chunk, F&& f) {
        refresh();
        World::LockGuard lock(*world_);
        // Prefix sums over archetype sizes give one flat index space.
        offsets_.clear();
        usize total = 0;
        for (Archetype* a : matches_) {
            offsets_.push_back(total);
            total += a->size();
        }
        if (total == 0) return;
        usize chunk = JobSystem::chunk_size(total, min_chunk);
        jobs.parallel_for(total, min_chunk, [&](usize begin, usize end) {
            usize chunk_index = begin / chunk;
            auto it = std::upper_bound(offsets_.begin(), offsets_.end(), begin);
            usize ai = static_cast<usize>(it - offsets_.begin()) - 1;
            while (begin < end) {
                Archetype* a = matches_[ai];
                usize local = begin - offsets_[ai];
                usize take = std::min(end - begin, a->size() - local);
                if (take > 0) {
                    std::tuple<detail::Column<Ts>...> cols{detail::Column<Ts>(*a)...};
                    const Entity* entities = a->entities().data();
                    for (usize i = local; i < local + take; ++i)
                        std::apply([&](auto&... c) { f(chunk_index, entities[i], c[i]...); }, cols);
                }
                begin += take;
                ++ai;
            }
        });
    }

    // Raw column access per archetype for hand-vectorized loops: f(std::span<const Entity>, Ts*...).
    template <class F> void each_archetype(F&& f) {
        refresh();
        World::LockGuard lock(*world_);
        for (Archetype* a : matches_) {
            if (a->empty()) continue;
            f(a->entities(), detail::Column<Ts>(*a).base...);
        }
    }

    usize count() {
        refresh();
        usize n = 0;
        for (Archetype* a : matches_) n += a->size();
        return n;
    }
    // The first matching entity, or null.
    Entity first() {
        refresh();
        for (Archetype* a : matches_)
            if (!a->empty()) return a->entities()[0];
        return null_entity;
    }

    const ComponentMask& includes() const { return include_; }
    const ComponentMask& excludes() const { return exclude_; }
    static ComponentMask reads() {
        ComponentMask m;
        ((std::is_const_v<Ts> ? m.set(component_id<Ts>()) : void()), ...);
        return m;
    }
    static ComponentMask writes() {
        ComponentMask m;
        ((!std::is_const_v<Ts> ? m.set(component_id<Ts>()) : void()), ...);
        return m;
    }

private:
    void refresh() {
        auto archetypes = world_->archetypes();
        for (; seen_ < archetypes.size(); ++seen_) {
            Archetype* a = archetypes[seen_].get();
            if (a->mask().contains(include_) && !a->mask().intersects(exclude_)) matches_.push_back(a);
        }
    }

    World* world_;
    ComponentMask include_, exclude_;
    std::vector<Archetype*> matches_;
    std::vector<usize> offsets_;
    usize seen_ = 0;
};

// --- World templates ------------------------------------------------------------------------------------------------

template <class... Ts> Query<Ts...> World::query(QueryFilter filter) { return Query<Ts...>(*this, filter); }

template <class... Ts, class F> void World::each(F&& f) { Query<Ts...>(*this).each(std::forward<F>(f)); }

template <class T> T* World::try_get(Entity e) const {
    if (!alive(e)) return nullptr;
    const Record& r = records_[e.index];
    using U = std::remove_const_t<T>;
    if constexpr (std::is_empty_v<U>) {
        static U tag{};
        return r.archetype->has(component_id<U>()) ? &tag : nullptr;
    } else {
        return static_cast<T*>(r.archetype->at(component_id<U>(), r.row));
    }
}

template <Component T, class... Args> T& World::add(Entity e, Args&&... args) {
    check_unlocked();
    PH_ASSERT(alive(e));
    ComponentId c = component_id<T>();
    Record& r = records_[e.index];
    if (r.archetype->has(c)) {
        T* existing = try_get<T>(e);
        *existing = T{std::forward<Args>(args)...};
        return *existing;
    }
    u32 row = move_entity(e, archetype_with(*r.archetype, c));
    T* p;
    if constexpr (std::is_empty_v<T>) p = try_get<T>(e);
    else p = ::new (records_[e.index].archetype->at(c, row)) T{std::forward<Args>(args)...};
    run_add_hooks(c, e);
    return *p;
}

template <class... Ts>
    requires(Component<std::decay_t<Ts>> && ...)
void World::insert(Entity e, Ts&&... components) {
    check_unlocked();
    PH_ASSERT(alive(e));
    Archetype* from = records_[e.index].archetype;
    ComponentMask added;
    ((from->has(component_id<std::decay_t<Ts>>()) ? void() : added.set(component_id<std::decay_t<Ts>>())), ...);
    if (!added.empty()) move_entity(e, archetype_for(from->mask() | added));
    u32 row = records_[e.index].row;
    Archetype* to = records_[e.index].archetype;
    auto place = [&]<class V>(V&& value) {
        using T = std::decay_t<V>;
        ComponentId c = component_id<T>();
        if constexpr (!std::is_empty_v<T>) {
            void* at = to->at(c, row);
            if (added.test(c)) ::new (at) T(std::forward<V>(value));
            else *static_cast<T*>(at) = std::forward<V>(value);
        }
    };
    (place(std::forward<Ts>(components)), ...);
    added.for_each([&](ComponentId c) { run_add_hooks(c, e); });
}

template <Component T> void World::remove(Entity e) {
    check_unlocked();
    if (!alive(e)) return;
    ComponentId c = component_id<T>();
    Record& r = records_[e.index];
    if (!r.archetype->has(c)) return;
    run_remove_hooks(c, e);
    move_entity(e, archetype_without(*records_[e.index].archetype, c));
}

template <class T, class... Args> T& World::set_resource(Args&&... args) {
    auto holder = std::make_unique<ResourceHolder<T>>(std::forward<Args>(args)...);
    T& ref = holder->value;
    resources_[type_id<T>()] = std::move(holder);
    return ref;
}

template <class T> T* World::try_resource() const {
    auto it = resources_.find(type_id<T>());
    return it == resources_.end() ? nullptr : &static_cast<ResourceHolder<T>*>(it->second.get())->value;
}

// --- CommandBuffer templates ----------------------------------------------------------------------------------------

template <class T> void CommandBuffer::AddCommand<T>::apply(World& w) {
    if (w.alive(e)) w.add<T>(e, std::move(value));
}
template <class... Ts> void CommandBuffer::InsertCommand<Ts...>::apply(World& w) {
    if (w.alive(e)) std::apply([&](auto&... vs) { w.insert(e, std::move(vs)...); }, values);
}
template <class T> void CommandBuffer::RemoveCommand<T>::apply(World& w) { w.remove<T>(e); }

} // namespace ph::ecs
