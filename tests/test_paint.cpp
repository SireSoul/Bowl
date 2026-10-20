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

TEST_CASE("paint: rounded boxes, polygons and curves") {
    auto square = points([](const Put& p) { rounded_box({0, 0}, {9, 9}, 0, true, p); });
    CHECK(square.size() == 100);
    auto rounded = points([](const Put& p) { rounded_box({0, 0}, {9, 9}, 3, true, p); });
    CHECK_FALSE(rounded.contains({0, 0})); // the corners are cut off
    CHECK(rounded.contains({0, 5}));
    CHECK(rounded.contains({5, 0}));
    CHECK(rounded.size() < 100);
    auto ring = points([](const Put& p) { rounded_box({0, 0}, {9, 9}, 3, false, p); });
    CHECK_FALSE(ring.contains({5, 5})); // an outline is hollow
    for (auto q : ring) CHECK(rounded.contains(q));

    // A triangle: its outline closes, and filling covers what's inside.
    std::vector<Point> tri{{0, 0}, {8, 0}, {0, 8}};
    auto edge = points([&](const Put& p) { polygon(tri, false, p); });
    auto solid = points([&](const Put& p) { polygon(tri, true, p); });
    CHECK(edge.contains({4, 4})); // the long side
    CHECK_FALSE(edge.contains({2, 2}));
    CHECK(solid.contains({2, 2}));
    CHECK_FALSE(solid.contains({7, 7}));
    for (auto q : edge) CHECK(solid.contains(q));

    // A curve starts and ends where it's told, passes through the point it's bent through, and has no gaps.
    Point a{0, 0}, b{20, 0}, through{10, 6};
    std::vector<Point> c;
    curve(a, control_through(a, b, through), b, [&](i32 x, i32 y) { c.push_back({x, y}); });
    CHECK(c.front() == a);
    CHECK(c.back() == b);
    CHECK(std::find(c.begin(), c.end(), through) != c.end());
    for (usize i = 1; i < c.size(); ++i) CHECK((std::abs(c[i].x - c[i - 1].x) <= 1 && std::abs(c[i].y - c[i - 1].y) <= 1));
}

TEST_CASE("paint: spray stays in its circle, and text is drawn in the pixel font") {
    u32 seed = 7;
    auto dots = points([&](const Put& p) { spray({10, 10}, 4, 30, seed, p); });
    CHECK(dots.size() > 10);
    for (auto [x, y] : dots) CHECK((x - 10) * (x - 10) + (y - 10) * (y - 10) <= 16);
    u32 again = 7;
    CHECK(points([&](const Put& p) { spray({10, 10}, 4, 30, again, p); }) == dots); // the same seed, the same spray

    // "HI": an H (3 wide), a gap, then an I.
    auto hi = points([](const Put& p) { text("HI", {0, 0}, 1, p); });
    CHECK(hi.contains({0, 0}));
    CHECK(hi.contains({2, 2})); // the H's bar
    CHECK_FALSE(hi.contains({1, 0}));
    CHECK_FALSE(hi.contains({3, 0})); // the space between letters
    CHECK(hi.contains({5, 1}));       // the I's stem
    CHECK(text_size("HI", 1) == Point{7, 5});
    CHECK(text_size("hi", 1) == Point{7, 5}); // lower case is drawn as upper case
    CHECK(text_size("A\nB", 2) == Point{6, 22});
    CHECK(points([](const Put& p) { text("A", {0, 0}, 2, p); }).size() == 4 * points([](const Put& p) { text("A", {0, 0}, 1, p); }).size());
    CHECK(text_size("I.I", 1).x < text_size("III", 1).x); // a full stop takes one column
}

TEST_CASE("paint: blur, jumble, scaling and ramps") {
    Image img(3, 3);
    img.set(1, 1, white);
    Pixel soft = blurred(img, 1, 1);
    CHECK(alpha(soft) < 255);
    CHECK(alpha(soft) > 0);
    CHECK(red(soft) == 255); // see-through neighbors don't darken it
    CHECK(blurred(Image(3, 3), 1, 1) == clear);
    u32 seed = 3;
    for (i32 i = 0; i < 20; ++i) {
        Pixel j = jumbled(img, 0, 0, seed);
        CHECK((j == clear || j == white));
    }
    Image big = scaled(img, 6, 6);
    CHECK(big.get(2, 2) == white);
    CHECK(big.get(3, 3) == white);
    CHECK(big.get(4, 4) == clear);
    CHECK(scaled(big, 3, 3) == img);
    auto r = ramp(rgba(0, 0, 0), rgba(255, 0, 0), 5);
    REQUIRE(r.size() == 5);
    CHECK(r.front() == rgba(0, 0, 0));
    CHECK(r.back() == rgba(255, 0, 0));
    for (usize i = 1; i < r.size(); ++i) CHECK(red(r[i]) > red(r[i - 1]));
    CHECK(to_hsv(ramp(rgba(255, 0, 0), rgba(255, 0, 255), 3)[1]).h > 290); // red to magenta goes the short way
}

TEST_CASE("paint: frames hold every layer's picture, undo, tags, and the sprite sheet") {
    Document d(4, 2);
    d.layer().image.set(0, 0, red_);
    d.commit("Paint");
    d.add_frame(true); // a copy of the first
    CHECK(d.frame_count() == 2);
    CHECK(d.frame() == 1);
    CHECK(d.layer().image.get(0, 0) == red_);
    d.layer().image.set(1, 0, blue_);
    d.commit("Paint");
    d.add_layer("Over");
    CHECK(d.cel(1, 0).width() == 4); // a new layer has a picture in every frame
    d.show_frame(0);
    CHECK(d.layers[0].image.get(1, 0) == clear); // the first frame never had the blue
    CHECK(d.composite(1).get(1, 0) == blue_);
    d.add_frame(false);
    CHECK(d.frame() == 1);
    CHECK(d.composite(1) == Image(4, 2)); // blank
    CHECK(d.composite(2).get(1, 0) == blue_);
    d.move_frame(1);
    CHECK(d.frame() == 2);
    CHECK(d.composite(1).get(1, 0) == blue_);
    d.remove_frame();
    CHECK(d.frame_count() == 2);
    d.undo();
    CHECK(d.frame_count() == 3);
    d.set_duration(0, 250);
    CHECK(d.duration(0) == 250);

    // Whole-picture changes reach every frame.
    d.flip(true);
    CHECK(d.cel(0, 0).get(3, 0) == red_);
    CHECK(d.cel(0, 1).get(2, 0) == blue_); // (moved: frame 1 holds it now)
    d.scale(8, 4);
    CHECK(d.width() == 8);
    CHECK(d.cel(0, 2).width() == 8);

    // The sheet: frames side by side, and JSON the engine's sprite sheets read.
    d.tags = {{"walk", 0, 2}};
    d.commit("Tag");
    Image sheet = d.sheet();
    CHECK(sheet.width() == 24);
    CHECK(sheet.height() == 4);
    CHECK(d.sheet(2).height() == 8);
    json::Value j = d.sheet_json("art/hero.png");
    CHECK(j["frames"].size() == 3);
    CHECK(j["frames"][1]["frame"]["x"].as_int() == 8);
    CHECK(j["frames"][0]["duration"].as_int() == 250);
    CHECK(j["meta"]["image"].as_string() == "art/hero.png");
    CHECK(j["meta"]["frameTags"][0]["name"].as_string() == "walk");
    CHECK(j["meta"]["frameTags"][0]["to"].as_int() == 2);

    // Saved and loaded with every frame, duration and tag.
    auto back = Document::from_json(d.to_json());
    REQUIRE(back);
    CHECK(back->frame_count() == 3);
    CHECK(back->duration(0) == 250);
    CHECK(back->tags == d.tags);
    for (usize f = 0; f < 3; ++f) CHECK(back->composite(f) == d.composite(f));

    // From frames (an Aseprite sheet cut up).
    Document made = Document::from_frames({Image(2, 2, red_), Image(2, 2, blue_)}, {50, 60}, {{"blink", 0, 1, "pingpong"}});
    CHECK(made.frame_count() == 2);
    CHECK(made.composite(1).get(0, 0) == blue_);
    CHECK(made.duration(1) == 60);
    CHECK_FALSE(made.modified());
}

TEST_CASE("paint: normal maps lean away from a shape's middle; emissive maps keep what glows") {
    paint::Image disc(17, 17); // clear around it: the picture's own edges don't bevel
    paint::ellipse({1, 1}, {15, 15}, true, [&](i32 x, i32 y) { disc.set(x, y, paint::rgba(120, 120, 120)); });
    paint::Image n = paint::normal_map(disc, {.bevel = 4, .brightness = 0});
    CHECK(paint::alpha(n.get(0, 0)) == 0); // outside the disc
    paint::Pixel left = n.get(2, 8), right = n.get(14, 8), top = n.get(8, 2), bottom = n.get(8, 14), middle = n.get(8, 8);
    CHECK(paint::red(left) < 110);
    CHECK(paint::red(right) > 146);
    CHECK(paint::green(top) > 146);   // green is up
    CHECK(paint::green(bottom) < 110);
    CHECK(std::abs(paint::red(middle) - 128) < 6);
    CHECK(std::abs(paint::green(middle) - 128) < 6);
    CHECK(paint::blue(middle) > 250);
    // Inverted: dents.
    paint::Image dent = paint::normal_map(disc, {.bevel = 4, .brightness = 0, .invert = true});
    CHECK(paint::red(dent.get(2, 8)) > 146);
    // A tile filling the picture stays flat at its edges.
    paint::Image tile(8, 8, paint::rgba(90, 90, 90));
    paint::Image flat = paint::normal_map(tile);
    CHECK(std::abs(paint::red(flat.get(0, 4)) - 128) < 3);

    paint::Image lamp(2, 1);
    lamp.set(0, 0, paint::rgba(255, 230, 120));
    lamp.set(1, 0, paint::rgba(40, 30, 30));
    paint::Image glow = paint::emissive_map(lamp, [](paint::Pixel p) { return paint::to_hsv(p).v > 0.8f; });
    CHECK(glow.get(0, 0) == lamp.get(0, 0));
    CHECK(glow.get(1, 0) == paint::clear);
}
