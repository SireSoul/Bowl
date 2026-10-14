#pragma once

#include "ph/core/jobs.hpp"
#include "ph/core/type_id.hpp"
#include "ph/ecs/world.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ph::ecs {

struct SystemContext {
    World& world;
    CommandBuffer& commands;
    JobSystem& jobs;
    f32 dt;
};

// An ordered list of systems. Systems that declare non-conflicting access run in parallel; the result is always the
// same as running them one by one in insertion order, because only systems with no data in common share a batch.
//
//     schedule.par_each<Position, const Velocity>("integrate", [](Position& p, const Velocity& v) { ... });
//     schedule.add("spawner", [](SystemContext& ctx) { ... });               // exclusive: runs alone
//     schedule.add("ai", fn).reads<Position>().writes<Brain>();              // declared access: may run in parallel
//
// Structural changes go through ctx.commands and are applied at barrier() points and at the end of run(), in system
// order — so results never depend on thread timing.
class Schedule : NonCopyable {
public:
    struct System {
        std::string name;
        std::function<void(SystemContext&)> fn;
        ComponentMask reads, writes;
        std::vector<TypeId> resource_reads, resource_writes;
        bool exclusive = true;
        bool barrier_after = false;
        bool enabled = true;
        f64 last_ms = 0;
        std::unique_ptr<CommandBuffer> commands;
    };

    class Builder {
    public:
        Builder(Schedule& s, usize index) : schedule_(&s), index_(index) {}
        template <class... Ts> Builder& reads() { sys().reads = sys().reads | ComponentMask::of<Ts...>(); return declared(); }
        template <class... Ts> Builder& writes() { sys().writes = sys().writes | ComponentMask::of<Ts...>(); return declared(); }
        template <class... Rs> Builder& reads_resource() { (sys().resource_reads.push_back(type_id<Rs>()), ...); return declared(); }
        template <class... Rs> Builder& writes_resource() { (sys().resource_writes.push_back(type_id<Rs>()), ...); return declared(); }
        // Run alone, after everything before it and before everything after it.
        Builder& exclusive() { sys().exclusive = true; schedule_->dirty_ = true; return *this; }

    private:
        System& sys() { return *schedule_->systems_[index_]; }
        Builder& declared() { sys().exclusive = false; schedule_->dirty_ = true; return *this; }
        Schedule* schedule_;
        usize index_;
    };

    Schedule() = default;

    // A system that may do anything; exclusive unless access is declared on the returned builder.
    Builder add(std::string name, std::function<void(SystemContext&)> fn);

    // A system over every entity matching Ts (const T = read-only). f(Ts&...), f(Entity, Ts&...) or
    // f(SystemContext&, Entity, Ts&...). Access is deduced from Ts.
    template <class... Ts, class F> Builder each(std::string name, F f, QueryFilter filter = {});
    // As each(), with the entities split across worker threads. Every chunk gets its own command buffer, merged in
    // chunk order, so commands stay deterministic.
    template <class... Ts, class F> Builder par_each(std::string name, F f, QueryFilter filter = {}, usize min_chunk = 512);

    // Applies every command recorded so far before later systems run.
    void barrier();

    void run(World& world, JobSystem& jobs, f32 dt);

    // Run everything on the calling thread, in order (to debug a suspected race).
    void set_parallel(bool parallel) { parallel_ = parallel; }
    bool parallel() const { return parallel_; }
    void set_enabled(std::string_view name, bool enabled);

    const std::vector<std::unique_ptr<System>>& systems() const { return systems_; }
    // Batches of system indices that run concurrently (for tools).
    const std::vector<std::vector<usize>>& batches();

private:
    bool conflicts(const System& a, const System& b) const;
    void build_batches();
    void run_system(System& s, World& world, JobSystem& jobs, f32 dt);
    void apply_commands(usize first, usize last);

    std::vector<std::unique_ptr<System>> systems_;
    std::vector<std::vector<usize>> batches_;
    bool dirty_ = true;
    bool parallel_ = true;
};

namespace detail {
template <class F, class... Ts> constexpr bool takes_context = std::is_invocable_v<F&, SystemContext&, Entity, Ts&...>;
}

template <class... Ts, class F> Schedule::Builder Schedule::each(std::string name, F f, QueryFilter filter) {
    auto query = std::make_shared<std::unique_ptr<Query<Ts...>>>();
    Builder b = add(std::move(name), [query, filter, fn = std::move(f)](SystemContext& ctx) mutable {
        if (!*query) *query = std::make_unique<Query<Ts...>>(ctx.world, filter);
        if constexpr (detail::takes_context<F, Ts...>) {
            (*query)->each([&](Entity e, Ts&... cs) { fn(ctx, e, cs...); });
        } else {
            (*query)->each(fn);
        }
    });
    b.template reads<>();
    auto& s = *systems_.back();
    s.reads = Query<Ts...>::reads() | filter.with;
    s.writes = Query<Ts...>::writes();
    return b;
}

template <class... Ts, class F>
Schedule::Builder Schedule::par_each(std::string name, F f, QueryFilter filter, usize min_chunk) {
    struct State {
        std::unique_ptr<Query<Ts...>> query;
        std::vector<CommandBuffer> chunk_commands;
    };
    auto state = std::make_shared<State>();
    Builder b = add(std::move(name), [state, filter, min_chunk, fn = std::move(f)](SystemContext& ctx) mutable {
        if (!state->query) state->query = std::make_unique<Query<Ts...>>(ctx.world, filter);
        if constexpr (detail::takes_context<F, Ts...>) {
            usize count = state->query->count();
            usize chunks = count == 0 ? 0 : (count + JobSystem::chunk_size(count, min_chunk) - 1) / JobSystem::chunk_size(count, min_chunk);
            while (state->chunk_commands.size() < chunks) state->chunk_commands.emplace_back(ctx.world);
            state->query->par_each_chunked(ctx.jobs, min_chunk, [&](usize chunk, Entity e, Ts&... cs) {
                SystemContext local{ctx.world, state->chunk_commands[chunk], ctx.jobs, ctx.dt};
                fn(local, e, cs...);
            });
            for (usize i = 0; i < chunks; ++i) ctx.commands.append(std::move(state->chunk_commands[i]));
        } else {
            state->query->par_each(ctx.jobs, fn, min_chunk);
        }
    });
    b.template reads<>();
    auto& s = *systems_.back();
    s.reads = Query<Ts...>::reads() | filter.with;
    s.writes = Query<Ts...>::writes();
    return b;
}

} // namespace ph::ecs
