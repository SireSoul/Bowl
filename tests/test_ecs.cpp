#include "ph/ecs/ecs.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>

using namespace ph;
using namespace ph::ecs;

namespace {

struct Position {
    f32 x = 0, y = 0;
};
struct Velocity {
    f32 x = 0, y = 0;
};
struct Frozen {}; // tag

// Counts live instances, to prove every constructed component is destroyed exactly once.
struct Tracked {
    static inline int alive = 0;
    std::unique_ptr<int> payload;
    explicit Tracked(int v = 0) : payload(std::make_unique<int>(v)) { ++alive; }
    Tracked(Tracked&& o) noexcept : payload(std::move(o.payload)) { ++alive; }
    Tracked& operator=(Tracked&& o) noexcept {
        payload = std::move(o.payload);
        return *this;
    }
    ~Tracked() { --alive; }
};

} // namespace

TEST_CASE("entities: create, destroy, generations") {
    World w;
    Entity a = w.create();
    Entity b = w.create();
    CHECK(w.alive(a));
    CHECK(w.alive(b));
    CHECK(a != b);
    CHECK(w.entity_count() == 2);

    w.destroy(a);
    CHECK_FALSE(w.alive(a));
    CHECK(w.entity_count() == 1);

    Entity c = w.create(); // reuses a's slot with a new generation
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK_FALSE(w.alive(a));
    CHECK(w.alive(c));
    CHECK_FALSE(w.alive(null_entity));
    w.destroy(a); // stale handle: no effect
    CHECK(w.alive(c));
}

TEST_CASE("components: add, get, replace, remove") {
    World w;
    Entity e = w.create(Position{1, 2});
    CHECK(w.has<Position>(e));
    CHECK_FALSE(w.has<Velocity>(e));
    CHECK(w.get<Position>(e).x == 1);

    w.add<Velocity>(e, 3.f, 4.f);
    CHECK(w.get<Velocity>(e).y == 4);
    CHECK(w.get<Position>(e).y == 2); // survived the archetype move

    w.add<Position>(e, 9.f, 9.f); // replace
    CHECK(w.get<Position>(e).x == 9);

    w.remove<Position>(e);
    CHECK_FALSE(w.has<Position>(e));
    CHECK(w.try_get<Position>(e) == nullptr);
    CHECK(w.get<Velocity>(e).x == 3);

    w.add<Frozen>(e);
    CHECK(w.has<Frozen>(e));
    CHECK(w.try_get<Frozen>(e) != nullptr);
}

TEST_CASE("components: non-trivial types are moved and destroyed exactly once") {
    Tracked::alive = 0;
    {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 1000; ++i) es.push_back(w.create(Tracked{i}, Position{}));
        CHECK(Tracked::alive == 1000);
        for (int i = 0; i < 1000; i += 2) w.add<Velocity>(es[i]); // forces archetype moves and column growth
        for (int i = 0; i < 1000; i += 3) w.destroy(es[i]);
        for (int i = 1; i < 1000; i += 3) w.remove<Tracked>(es[i]);
        int expected = 0;
        for (int i = 0; i < 1000; ++i)
            if (i % 3 == 2) {
                ++expected;
                REQUIRE(w.has<Tracked>(es[i]));
                CHECK(*w.get<Tracked>(es[i]).payload == i);
            }
        CHECK(Tracked::alive == expected);
    }
    CHECK(Tracked::alive == 0);
}

TEST_CASE("queries: include, exclude, const, entity parameter") {
    World w;
    for (int i = 0; i < 100; ++i) {
        Entity e = w.create(Position{static_cast<f32>(i), 0});
        if (i % 2 == 0) w.add<Velocity>(e, 1.f, 0.f);
        if (i % 5 == 0) w.add<Frozen>(e);
    }
    CHECK(w.query<Position>().count() == 100);
    CHECK(w.query<Position, Velocity>().count() == 50);
    CHECK(w.query<Position, Velocity>(without<Frozen>()).count() == 40);
    CHECK(w.query<Position>(with<Frozen>()).count() == 20);

    auto moving = w.query<Position, const Velocity>(without<Frozen>());
    moving.each([](Position& p, const Velocity& v) { p.x += v.x; });
    f32 sum = 0;
    w.each<const Position>([&](Entity e, const Position& p) {
        CHECK(w.alive(e));
        sum += p.x;
    });
    CHECK(sum == doctest::Approx(4950 + 40));
}

TEST_CASE("queries: cached query sees archetypes created later") {
    World w;
    auto q = w.query<Position>();
    CHECK(q.count() == 0);
    w.create(Position{});
    w.create(Position{}, Name{"x"}); // new archetype after the query was made
    CHECK(q.count() == 2);
    CHECK(q.first() != null_entity);
}

TEST_CASE("queries: parallel iteration touches every entity once") {
    World w;
    JobSystem jobs(4);
    for (int i = 0; i < 20000; ++i) {
        Entity e = w.create(Position{}, Velocity{1, 0});
        if (i % 3 == 0) w.add<Name>(e); // spread over several archetypes
        if (i % 7 == 0) w.add<Frozen>(e);
    }
    auto q = w.query<Position, const Velocity>();
    q.par_each(jobs, [](Position& p, const Velocity& v) { p.x += v.x; }, 64);
    usize ones = 0;
    w.each<const Position>([&](const Position& p) { ones += p.x == 1.0f; });
    CHECK(ones == 20000);
}

TEST_CASE("structural changes while iterating are rejected; commands are applied in order") {
    World w;
    for (int i = 0; i < 10; ++i) w.create(Position{static_cast<f32>(i), 0});
    CommandBuffer cmds(w);
    Entity spawned = null_entity;
    w.each<const Position>([&](Entity e, const Position& p) {
        CHECK(w.locked());
        if (p.x < 5) cmds.destroy(e);
        else cmds.add<Velocity>(e, p.x, 0.f);
        if (p.x == 0) spawned = cmds.create(Name{"spawned"}, Position{100, 0});
    });
    CHECK_FALSE(w.locked());
    CHECK_FALSE(w.alive(spawned)); // reserved, not yet alive
    CHECK(w.entity_count() == 10);
    cmds.apply();
    CHECK(w.entity_count() == 6);
    CHECK(w.query<Velocity>().count() == 5);
    REQUIRE(w.alive(spawned));
    CHECK(w.get<Name>(spawned).value == "spawned");
    CHECK(w.get<Position>(spawned).x == 100);
}

TEST_CASE("commands: reserved ids are unique across buffers and reuse free slots") {
    World w;
    std::vector<Entity> old;
    for (int i = 0; i < 8; ++i) old.push_back(w.create());
    for (int i = 0; i < 4; ++i) w.destroy(old[i]);
    CommandBuffer a(w), b(w);
    std::set<u32> indices;
    for (int i = 0; i < 6; ++i) {
        indices.insert(a.create().index);
        indices.insert(b.create().index);
    }
    CHECK(indices.size() == 12);
    a.apply();
    b.apply();
    CHECK(w.entity_count() == 4 + 12);
    for (Entity e : old) {
        if (e.index < old[4].index) CHECK_FALSE(w.alive(e));
    }
}

TEST_CASE("hooks: on_add and on_remove") {
    World w;
    int added = 0, removed = 0;
    w.on_add<Name>([&](World& world, Entity e) {
        ++added;
        CHECK(world.has<Name>(e));
    });
    w.on_remove<Name>([&](World& world, Entity e) {
        ++removed;
        CHECK(world.has<Name>(e)); // still readable during the hook
    });
    Entity a = w.create(Name{"a"});
    Entity b = w.create();
    w.add<Name>(b, "b");
    w.add<Name>(b, "b2"); // replace: no hook
    CHECK(added == 2);
    w.remove<Name>(a);
    w.destroy(b);
    CHECK(removed == 2);
}

TEST_CASE("resources") {
    World w;
    CHECK(w.try_resource<int>() == nullptr);
    w.set_resource<int>(42);
    CHECK(w.resource<int>() == 42);
    w.resource<int>() = 7;
    CHECK(*w.try_resource<int>() == 7);
    w.remove_resource<int>();
    CHECK(w.try_resource<int>() == nullptr);
}

TEST_CASE("const and non-const component types share one id") {
    CHECK(component_id<Position>() == component_id<const Position>());
    CHECK(component_info(component_id<Position>()).name.find("Position") != std::string_view::npos);
    CHECK(component_info(component_id<Frozen>()).size == 0);
}
