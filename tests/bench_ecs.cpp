// ECS throughput. Build Release and run: bowl_bench_ecs [entities]

#include "bowl/core/log.hpp"
#include "bowl/core/time.hpp"
#include "bowl/ecs/ecs.hpp"

#include <cstdlib>
#include <string>

using namespace bowl;
using namespace bowl::ecs;

namespace {
struct Position {
    f32 x, y;
};
struct Velocity {
    f32 x, y;
};
struct Health {
    i32 value;
};
struct Enemy {};

template <class F> f64 time_ms(F&& f, int repeats = 10) {
    f64 best = 1e30;
    for (int i = 0; i < repeats; ++i) {
        Stopwatch w;
        f();
        best = std::min(best, w.milliseconds());
    }
    return best;
}
} // namespace

int main(int argc, char** argv) {
    usize n = argc > 1 ? static_cast<usize>(std::atoll(argv[1])) : 1'000'000;
    JobSystem jobs;
    info("ECS benchmark: {} entities, {} threads", n, jobs.concurrency());

    World world;
    f64 create_ms = time_ms(
        [&] {
            World w;
            for (usize i = 0; i < n; ++i) w.create(Position{0, 0}, Velocity{1, 1});
        },
        3);
    info("  create {} entities with 2 components: {:.2f} ms ({:.1f} ns/entity)", n, create_ms, create_ms * 1e6 / static_cast<f64>(n));

    for (usize i = 0; i < n; ++i) {
        Entity e = world.create(Position{0, 0}, Velocity{1, 1});
        if (i % 2) world.add<Health>(e, 100);
        if (i % 3 == 0) world.add<Enemy>(e);
    }
    auto q = world.query<Position, const Velocity>();
    f64 iter_ms = time_ms([&] { q.each([](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; }); });
    info("  iterate Position+Velocity (4 archetypes):  {:.2f} ms ({:.2f} ns/entity)", iter_ms, iter_ms * 1e6 / static_cast<f64>(n));

    f64 par_ms = time_ms([&] { q.par_each(jobs, [](Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; }); });
    info("  par_each same query:                       {:.2f} ms ({:.1f}x)", par_ms, iter_ms / par_ms);

    auto heavy = world.query<Position, const Velocity, Health>();
    f64 heavy_ms = time_ms([&] {
        heavy.par_each(jobs, [](Position& p, const Velocity& v, Health& h) {
            for (int k = 0; k < 16; ++k) p.x = p.x * 0.999f + v.x * 0.001f + static_cast<f32>(h.value) * 1e-6f;
        });
    });
    info("  par_each heavier per-entity work (n/2):     {:.2f} ms", heavy_ms);

    std::vector<Entity> all;
    world.each<const Position>([&](Entity e, const Position&) { all.push_back(e); });
    f64 add_remove_ms = time_ms(
        [&] {
            for (usize i = 0; i < all.size(); i += 10) world.add<Health>(all[i], 1);
            for (usize i = 0; i < all.size(); i += 10) world.remove<Health>(all[i]);
        },
        3);
    info("  add+remove a component on {} entities:    {:.2f} ms", all.size() / 10, add_remove_ms);
    return 0;
}
