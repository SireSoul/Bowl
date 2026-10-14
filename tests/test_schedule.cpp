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
        // Component data and iteration order are deterministic. Entity ids reserved from parallel chunks are unique
        // but depend on thread timing, so they're left out here (see the sequential test below for ids).
        std::vector<int> out;
        w.each<const A>([&](const A& a) { out.push_back(a.v); });
        return out;
    };
    CHECK(simulate(true) == simulate(false));
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
