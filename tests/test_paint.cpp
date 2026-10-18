// Bowl Editor's painting: shapes that match what pixel artists expect, fills and the magic wand, layers, selections,
// moving, the canvas, and undo.

#include "bowl/paint/paint.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace bowl;
using namespace bowl::paint;

namespace {

constexpr Pixel red_ = rgba(255, 0, 0), blue_ = rgba(0, 0, 255), white = rgba(255, 255, 255);

std::set<std::pair<i32, i32>> points(const std::function<void(const Put&)>& draw) {
    std::set<std::pair<i32, i32>> out;
    draw([&](i32 x, i32 y) { out.insert({x, y}); });
    return out;
}

} // namespace

TEST_CASE("paint: lines have no gaps, snap to the useful angles, and the iso slope is exact stairs") {
    auto l = points([](const Put& put) { line({0, 0}, {7, 3}, put); });
    CHECK(l.size() == 8); // one pixel per column
    CHECK(snap_line({0, 0}, {10, 1}) == Point{10, 0});
    CHECK(snap_line({0, 0}, {9, 10}) == Point{10, 10});
    CHECK(snap_line({0, 0}, {11, 5}) == Point{10, 5}); // 2:1
    auto iso = points([](const Put& put) { line({0, 0}, {8, 4}, put); });
    for (i32 x = 0; x <= 8; ++x) CHECK(iso.contains({x, x / 2})); // two across, one down
}

TEST_CASE("paint: boxes, iso diamonds and ellipses") {
    CHECK(points([](const Put& p) { box({0, 0}, {3, 2}, false, false, p); }).size() == 10);
    CHECK(points([](const Put& p) { box({0, 0}, {3, 2}, true, false, p); }).size() == 12);
    auto diamond = points([](const Put& p) { box({0, 0}, {16, 8}, false, true, p); });
    CHECK(diamond.contains({8, 0}));
    CHECK(diamond.contains({16, 4}));
    CHECK(diamond.contains({8, 8}));
    CHECK(diamond.contains({0, 4}));
    CHECK_FALSE(diamond.contains({0, 0}));
    auto ring = points([](const Put& p) { ellipse({0, 0}, {9, 5}, false, p); });
    auto disc = points([](const Put& p) { ellipse({0, 0}, {9, 5}, true, p); });
    CHECK(ring.contains({0, 2}));
    CHECK(ring.contains({9, 3}));
    CHECK_FALSE(ring.contains({0, 0}));
    CHECK(disc.size() > ring.size());
    for (auto p : ring) CHECK(disc.contains(p));
    // Every row of the ellipse is inside the box.
    for (auto [x, y] : disc) CHECK((x >= 0 && x <= 9 && y >= 0 && y <= 5));
}

TEST_CASE("paint: pixel-perfect strokes lose their L corners, brushes are round, dither thins them") {
    auto pp = pixel_perfect({{0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2}});
    CHECK(pp == std::vector<Point>{{0, 0}, {1, 1}, {2, 2}});
    CHECK(points([](const Put& p) { dab({.size = 3}, 5, 5, 20, 20, p); }).size() == 5); // a plus
    CHECK(points([](const Put& p) { dab({.size = 3, .square = true}, 5, 5, 20, 20, p); }).size() == 9);
    auto half = points([](const Put& p) { dab({.size = 4, .square = true, .density = 8}, 5, 5, 20, 20, p); });
    CHECK(half.size() == 8);
    auto mirrored = points([](const Put& p) { dab({.size = 1, .mirror_x = true}, 2, 3, 10, 10, p); });
    CHECK(mirrored == std::set<std::pair<i32, i32>>{{2, 3}, {7, 3}});
}

TEST_CASE("paint: fills and the magic wand find connected colors, or every such color") {
    Image img(5, 5, white);
    for (i32 y = 0; y < 5; ++y) img.set(2, y, red_); // a wall down the middle
    img.set(4, 4, rgba(250, 250, 250));
    CHECK(region(img, 0, 0, 0, true).count() == 10);
    CHECK(region(img, 0, 0, 0, false).count() == 19);  // the right side too, but not the near-white
    CHECK(region(img, 0, 0, 10, false).count() == 20); // near enough
}

TEST_CASE("paint: gradients, shading and adjustments") {
    Image img(9, 1);
    gradient(img, {}, {0, 0}, {8, 0}, rgba(0, 0, 0), rgba(200, 200, 200), false);
    CHECK(red(img.get(0, 0)) == 0);
    CHECK(red(img.get(4, 0)) == 100);
    CHECK(red(img.get(8, 0)) == 200);
    gradient(img, {}, {0, 0}, {8, 0}, red_, blue_, true);
    for (Pixel p : img.pixels()) CHECK((p == red_ || p == blue_));
    CHECK(img.get(0, 0) == red_);
    CHECK(img.get(8, 0) == blue_);
    // Shading: along a ramp when the color is in it, otherwise lighter and darker.
    std::vector<Pixel> ramp{rgba(40, 20, 20), rgba(120, 60, 60), rgba(200, 120, 120)};
    CHECK(shade(rgba(120, 60, 60), 1, ramp) == rgba(200, 120, 120));
    CHECK(shade(rgba(40, 20, 20), -1, ramp) == rgba(40, 20, 20));
    CHECK(to_hsv(shade(rgba(100, 140, 100), 1)).v > to_hsv(rgba(100, 140, 100)).v);
    CHECK(to_hsv(shade(rgba(100, 140, 100), -1)).v < to_hsv(rgba(100, 140, 100)).v);
    // Hue and back.
    for (Pixel p : {red_, rgba(10, 200, 30), rgba(77, 77, 77), rgba(200, 100, 250)}) {
        Pixel q = from_hsv(to_hsv(p));
        CHECK(std::abs(red(p) - red(q)) <= 1);
        CHECK(std::abs(blue(p) - blue(q)) <= 1);
    }
    Image dot(3, 3);
    dot.set(1, 1, red_);
    outline(dot, {}, blue_, false);
    CHECK(dot.get(1, 0) == blue_);
    CHECK(dot.get(0, 0) == clear);
    CHECK(painted_bounds(dot).w == 3);
    CHECK(flipped(turned(dot, true), true).get(1, 1) == red_);
}

TEST_CASE("paint: layers blend in order, and undo walks back through every kind of change") {
    Document d(4, 4);
    CHECK(d.history().size() == 1);
    d.layer().image.set(0, 0, red_);
    d.commit("Pencil");
    d.add_layer("Top");
    d.layer().image.set(0, 0, rgba(0, 0, 255, 128));
    d.commit("Pencil");
    Pixel mixed = d.composite().get(0, 0);
    CHECK(red(mixed) > 100);
    CHECK(blue(mixed) > 100);
    d.layer().opacity = 0; // a see-through layer shows nothing
    CHECK(d.composite().get(0, 0) == red_);
    d.layer().opacity = 1;
    d.merge_down();
    CHECK(d.layers.size() == 1);
    CHECK(d.layer().image.get(0, 0) == mixed);
    CHECK(d.undo());
    CHECK(d.layers.size() == 2);
    CHECK(d.undo());
    CHECK(d.undo());
    CHECK(d.layers.size() == 1);
    CHECK(d.redo());
    CHECK(d.layers.size() == 2);
    CHECK(d.modified());
    d.mark_saved();
    CHECK_FALSE(d.modified());
}

TEST_CASE("paint: selections limit tools, and moving lifts and puts pixels down") {
    Document d(6, 6);
    d.layer().image.set(1, 1, red_);
    d.layer().image.set(4, 4, blue_);
    d.commit("Pencil");
    d.select_box({0, 0, 3, 3}, Mask::Mode::Replace);
    CHECK(d.editable(1, 1));
    CHECK_FALSE(d.editable(4, 4));
    d.select_box({1, 1, 1, 1}, Mask::Mode::Subtract);
    CHECK_FALSE(d.editable(1, 1));
    d.select_box({0, 0, 3, 3}, Mask::Mode::Replace);
    d.lift();
    REQUIRE(d.floating);
    d.floating->x += 2;
    CHECK(d.composite().get(3, 1) == red_); // shown where it's being moved
    d.put_down();
    CHECK(d.layer().image.get(1, 1) == clear);
    CHECK(d.layer().image.get(3, 1) == red_);
    CHECK(d.history().back() == "Move");
    d.undo();
    CHECK(d.layer().image.get(1, 1) == red_);
    // Copy and paste.
    Point at;
    Image copied = d.copy(&at);
    d.select_none();
    d.paste(copied, {3, 3});
    d.put_down();
    CHECK(d.layer().image.get(4, 4) == red_);
}

TEST_CASE("paint: the canvas grows from an anchor, crops, trims and flips") {
    Document d(4, 4);
    d.layer().image.set(3, 3, red_);
    d.commit("Pencil");
    d.resize(8, 6, 0.5f, 1); // grow around the bottom middle: the foot stays put
    CHECK(d.width() == 8);
    CHECK(d.height() == 6);
    CHECK(d.layer().image.get(5, 5) == red_);
    Box kept = d.trim();
    CHECK(kept.x == 5);
    CHECK(d.width() == 1);
    CHECK(d.history().back() == "Trim");
    d.undo();
    d.flip(true);
    CHECK(d.layer().image.get(2, 5) == red_);
}

TEST_CASE("paint: layers save and load, every pixel and setting kept") {
    Document d(3, 2);
    d.layer().image.set(2, 1, rgba(1, 2, 3, 4));
    d.add_layer("Shading");
    d.layer().opacity = 0.5f;
    d.layer().locked = true;
    auto back = Document::from_json(d.to_json());
    REQUIRE(back);
    CHECK(back->layers.size() == 2);
    CHECK(back->layers[0].image.get(2, 1) == rgba(1, 2, 3, 4));
    CHECK(back->layers[1].name == "Shading");
    CHECK(back->layers[1].opacity == doctest::Approx(0.5f));
    CHECK(back->layers[1].locked);
    CHECK(unbase64(base64({1, 2, 3, 250, 251})) == std::vector<u8>{1, 2, 3, 250, 251});
    CHECK_FALSE(Document::from_json(json::parse(R"({"width": 2, "height": 2, "layers": [{"rgba": "AAAA"}]})").value));
}
