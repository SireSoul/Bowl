#include "ph/ecs/ecs.hpp"

#include <doctest/doctest.h>

using namespace ph;
using namespace ph::ecs;

namespace {
struct A {
    int v = 0;
};
struct B {
    int v = 0;
};
struct C {
    int v = 0;
};
struct Counter {
    int value = 0;
};
} // namespace

TEST_CASE("schedule: non-conflicting systems share a batch, conflicting ones don't") {
    Schedule s;
    s.par_each<A>("write A", [](A&) {});
    s.par_each<const B>("read B", [](const B&) {});
    s.par_each<const B, C>("read B write C", [](const B&, C&) {});
    s.par_each<const A>("read A", [](const A&) {}); // conflicts with "write A"
    s.add("exclusive", [](SystemContext&) {});
    s.par_each<B>("write B", [](B&) {});
    const auto& batches = s.batches();
    REQUIRE(batches.size() == 4);
    CHECK(batches[0] == std::vector<usize>{0, 1, 2});
    CHECK(batches[1] == std::vector<usize>{3});
    CHECK(batches[2] == std::vector<usize>{4});
    CHECK(batches[3] == std::vector<usize>{5});
}

TEST_CASE("schedule: resource access participates in conflicts") {
    Schedule s;
    s.add("r1", [](SystemContext&) {}).reads_resource<Counter>();
    s.add("r2", [](SystemContext&) {}).reads_resource<Counter>();
    s.add("w", [](SystemContext&) {}).writes_resource<Counter>();
    const auto& batches = s.batches();
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].size() == 2);
}

TEST_CASE("schedule: barrier splits batches and applies commands") {
    World w;
    JobSystem jobs(2);
    w.create(A{1});
    Schedule s;
    s.each<const A>("spawn B", [](SystemContext& ctx, Entity e, const A&) { ctx.commands.add<B>(e, 5); });
    s.barrier();
    int seen = 0;
    s.each<const B>("see B", [&](const B& b) { seen += b.v; });
    s.run(w, jobs, 0.016f);
    CHECK(seen == 5);
}

TEST_CASE("schedule: results are identical with and without parallelism") {
    auto simulate = [](bool parallel) {
        World w;
        JobSystem jobs(parallel ? 4 : 0);
        for (int i = 0; i < 5000; ++i) w.create(A{i}, B{i * 2});
        Schedule s;
        s.set_parallel(parallel);
        s.par_each<A, const B>("a += b", [](A& a, const B& b) { a.v += b.v; });
        s.par_each<C>("c", [](C& c) { c.v++; });
        // Despawn and spawn from parallel chunks; merged in chunk order.
        s.par_each<const A>("churn", [](SystemContext& ctx, Entity e, const A& a) {
            if (a.v % 7 == 0) ctx.commands.destroy(e);
            if (a.v % 11 == 0) ctx.commands.create(A{a.v + 1}, B{1});
        });
        for (int frame = 0; frame < 5; ++frame) s.run(w, jobs, 1.0f / 60);
        // Component data, iteration order and entity ids are all deterministic: ids created from parallel chunks are
        // stand-ins until the chunks' commands are applied, in chunk order.
        std::vector<std::pair<u64, int>> out;
        w.each<const A>([&](Entity e, const A& a) { out.emplace_back(e.bits(), a.v); });
        return out;
    };
    auto expected = simulate(false);
    for (int run = 0; run < 5; ++run) CHECK(simulate(true) == expected);
}

namespace {
struct Link { // a reflected component pointing at other entities
    Entity to;
    std::vector<Entity> also;
};
PH_REFLECT(Link, to, also)
} // namespace

TEST_CASE("schedule: stand-in ids from parallel systems become real ones, inside components too") {
    auto simulate = [](int workers) {
        World w;
        JobSystem jobs(workers);
        for (int i = 0; i < 3000; ++i) w.create(A{i});
        Schedule s;
        s.par_each<const A>("spawn pairs", [](SystemContext& ctx, Entity e, const A& a) {
            if (a.v % 5 != 0) return;
            Entity first = ctx.commands.create(B{a.v});
            // The second points at the first (a stand-in) and at its spawner (a real entity).
            ctx.commands.create(C{a.v}, Link{first, {e, first}});
        });
        s.run(w, jobs, 1.0f / 60);
        std::vector<std::tuple<u64, u64, u64, u64>> links;
        bool right = true;
        w.each<const Link, const C>([&](Entity e, const Link& l, const C& c) {
            right = right && w.alive(l.to) && !l.to.is_placeholder() && w.get<B>(l.to).v == c.v && l.also.size() == 2 &&
                    l.also[1] == l.to && w.get<A>(l.also[0]).v == c.v;
            links.emplace_back(e.bits(), l.to.bits(), l.also[0].bits(), l.also[1].bits());
        });
        CHECK(right); // each points at its own partner and spawner
        CHECK(links.size() == 600);
        return links;
    };
    auto expected = simulate(0);
    for (int run = 0; run < 5; ++run) CHECK(simulate(4) == expected);

    // Outside a schedule, a buffer hands out real ids as before.
    World w;
    CommandBuffer cmd(w);
    Entity e = cmd.create(A{1});
    CHECK_FALSE(e.is_placeholder());
    cmd.apply();
    CHECK(w.get<A>(e).v == 1);
}

TEST_CASE("schedule: entities spawned from sequential systems get deterministic ids") {
    auto simulate = [](int workers) {
        World w;
        JobSystem jobs(workers);
        for (int i = 0; i < 500; ++i) w.create(A{i});
        Schedule s;
        s.each<const A>("churn", [](SystemContext& ctx, Entity e, const A& a) {
            if (a.v % 7 == 0) ctx.commands.destroy(e);
            if (a.v % 11 == 0) ctx.commands.create(A{a.v + 1});
        });
        s.par_each<A>("bump", [](A& a) { a.v += 3; }); // runs in parallel, no structural changes
        for (int frame = 0; frame < 5; ++frame) s.run(w, jobs, 1.0f / 60);
        std::vector<std::pair<u64, int>> out;
        w.each<const A>([&](Entity e, const A& a) { out.emplace_back(e.bits(), a.v); });
        return out;
    };
    CHECK(simulate(4) == simulate(0));
}
