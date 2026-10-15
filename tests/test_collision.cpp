#include "ph/physics/collision.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace ph;
using namespace ph::physics;

TEST_CASE("collision: overlap and minimum translation") {
    Rect a{0, 0, 10, 10};
    CHECK(overlaps(a, Rect{5, 5, 10, 10}));
    CHECK_FALSE(overlaps(a, Rect{10, 0, 5, 5})); // touching isn't overlapping

    Contact c = collide(Rect{8, 2, 10, 4}, a); // pokes 2 into a's right side
    REQUIRE(c.hit);
    CHECK(c.normal == Vec2{1, 0});
    CHECK(c.depth == doctest::Approx(2));

    Contact cc = collide(Circle{{3, 0}, 2}, Circle{{0, 0}, 2});
    REQUIRE(cc.hit);
    CHECK(cc.normal.x == doctest::Approx(1));
    CHECK(cc.depth == doctest::Approx(1));

    Contact cr = collide(Circle{{5, -1}, 2}, a); // circle above the box, 1 unit into it
    REQUIRE(cr.hit);
    CHECK(cr.normal.y == doctest::Approx(-1));
    CHECK(cr.depth == doctest::Approx(1));
    Contact inside = collide(Circle{{1, 5}, 1}, a); // center inside, nearest side is the left
    REQUIRE(inside.hit);
    CHECK(inside.normal == Vec2{-1, 0});
    CHECK_FALSE(collide(Circle{{20, 20}, 1}, a).hit);
}

TEST_CASE("collision: swept boxes and rays") {
    Rect wall{10, -100, 2, 200};
    SweepHit h = sweep(Rect{0, 0, 2, 2}, {20, 0}, wall);
    REQUIRE(h.hit);
    CHECK(h.time == doctest::Approx(8.0f / 20.0f)); // right edge reaches x=10 after 8 units
    CHECK(h.normal == Vec2{-1, 0});
    CHECK_FALSE(sweep(Rect{0, 0, 2, 2}, {5, 0}, wall).hit);    // stops short
    CHECK_FALSE(sweep(Rect{0, 0, 2, 2}, {-20, 0}, wall).hit);  // moving away
    CHECK_FALSE(sweep(Rect{8, 0, 2, 2}, {0, 50}, wall).hit);   // sliding along, touching
    // Fast enough to skip over the wall in one step: still caught.
    CHECK(sweep(Rect{0, 0, 2, 2}, {1000, 0}, wall).hit);

    RayHit r = raycast({0, 5}, {1, 0}, 100, Rect{10, 0, 10, 10});
    REQUIRE(r.hit);
    CHECK(r.distance == doctest::Approx(10));
    CHECK(r.point == Vec2{10, 5});
    CHECK_FALSE(raycast({0, 5}, {1, 0}, 5, Rect{10, 0, 10, 10}).hit);
    CHECK_FALSE(raycast({15, 5}, {1, 0}, 100, Rect{10, 0, 10, 10}).hit); // starts inside
}

TEST_CASE("collision: move_and_slide slides along walls and never tunnels") {
    // A 1-unit grid; column x=5 is a wall, row y=8 is a floor.
    auto solid = [](i32 x, i32 y) { return x == 5 || y == 8; };
    GridSolids grid{{1, 1}, {0, 0}, solid};

    Rect box{1, 1, 0.8f, 0.8f};
    MoveResult m = move_and_slide(box, {10, 3}, grid); // diagonally into the wall
    CHECK(m.collided);
    CHECK(box.right() <= 5.0f);
    CHECK(box.right() == doctest::Approx(5).epsilon(0.001));
    CHECK(box.y == doctest::Approx(4).epsilon(0.001)); // slid down along it the whole way
    CHECK(m.last_normal == Vec2{-1, 0});

    Rect fast{1, 1, 0.8f, 0.8f};
    move_and_slide(fast, {0, 1000}, grid); // would cross the floor in one step
    CHECK(fast.bottom() <= 8.0f);
    CHECK(fast.bottom() == doctest::Approx(8).epsilon(0.001));

    Rect corner{3, 6, 0.8f, 0.8f};
    move_and_slide(corner, {5, 5}, grid); // into the corner where wall meets floor
    CHECK(corner.right() <= 5.0f);
    CHECK(corner.bottom() <= 8.0f);

    Rect free_box{0, 0, 1, 1};
    MoveResult open = move_and_slide(free_box, {2, 2}, [](const Rect&, FunctionRef<void(const Rect&)>) {});
    CHECK_FALSE(open.collided);
    CHECK(open.moved == Vec2{2, 2});
}

TEST_CASE("collision: spatial hash reports each overlapping item once") {
    SpatialHash<int> hash(16);
    hash.insert(1, {0, 0, 10, 10});
    hash.insert(2, {30, 30, 40, 40}); // spans many cells
    hash.insert(3, {100, 100, 5, 5});
    std::multiset<int> found;
    hash.query({5, 5, 40, 40}, [&](int id, const Rect&) { found.insert(id); });
    CHECK(found == std::multiset<int>{1, 2});
    found.clear();
    hash.query({-50, -50, 10, 10}, [&](int id, const Rect&) { found.insert(id); }); // negative cells
    CHECK(found.empty());
    hash.clear();
    hash.query({0, 0, 200, 200}, [&](int id, const Rect&) { found.insert(id); });
    CHECK(found.empty());
}
