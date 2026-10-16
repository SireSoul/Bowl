#include "bowl/atlas/atlas.hpp"
#include "bowl/gfx/sprite_sheet.hpp"

#include <doctest/doctest.h>

using namespace bowl;
using namespace bowl::gfx;

namespace {

// Trimmed down from a real Aseprite 1.3 "Export Sprite Sheet" (array layout).
constexpr const char* aseprite_json = R"({
  "frames": [
    { "filename": "hero 0.aseprite", "frame": { "x": 0, "y": 0, "w": 16, "h": 24 }, "rotated": false, "trimmed": true,
      "spriteSourceSize": { "x": 8, "y": 8, "w": 16, "h": 24 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 100 },
    { "filename": "hero 1.aseprite", "frame": { "x": 16, "y": 0, "w": 32, "h": 32 }, "rotated": false, "trimmed": false,
      "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 100 },
    { "filename": "hero 2.aseprite", "frame": { "x": 48, "y": 0, "w": 32, "h": 32 }, "rotated": false, "trimmed": false,
      "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 200 },
    { "filename": "hero 3.aseprite", "frame": { "x": 80, "y": 0, "w": 32, "h": 32 }, "rotated": false, "trimmed": false,
      "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 100 }
  ],
  "meta": {
    "app": "https://www.aseprite.org/", "image": "hero.png", "format": "RGBA8888", "size": { "w": 112, "h": 32 },
    "frameTags": [
      { "name": "idle", "from": 0, "to": 1, "direction": "forward", "color": "#000000ff" },
      { "name": "swing", "from": 1, "to": 3, "direction": "forward", "repeat": "1", "color": "#000000ff" },
      { "name": "bob", "from": 0, "to": 2, "direction": "pingpong", "color": "#000000ff" }
    ]
  }
})";

SpriteSheet parsed() {
    SpriteSheet sheet;
    std::string image, err;
    auto doc = json::parse(aseprite_json);
    REQUIRE(doc);
    REQUIRE(SpriteSheet::parse_aseprite(doc.value, sheet, image, err));
    CHECK(image == "hero.png");
    return sheet;
}

} // namespace

TEST_CASE("sprite sheet: Aseprite frames, trimming and tags") {
    SpriteSheet sheet = parsed();
    REQUIRE(sheet.frames.size() == 4);
    CHECK(sheet.frames[0].offset == Vec2{8, 8});
    CHECK(sheet.frames[0].size == Vec2{32, 32});
    CHECK(sheet.frames[2].duration == doctest::Approx(0.2f));
    REQUIRE(sheet.animations.size() == 3);
    CHECK(sheet.find("swing")->repeat == 1);
    CHECK(sheet.find("bob")->direction == SpriteAnimation::Direction::PingPong);
    CHECK(sheet.find("nope") == nullptr);
}

TEST_CASE("sprite sheet: rejects documents without frames") {
    SpriteSheet sheet;
    std::string image, err;
    CHECK_FALSE(SpriteSheet::parse_aseprite(json::parse(R"({"meta": {}})").value, sheet, image, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("animator: loops, per-frame durations, one-shots and ping-pong") {
    AssetManager assets;
    SpriteAnimator anim;
    anim.sheet = assets.add("hero", std::make_unique<SpriteSheet>(parsed()));

    REQUIRE(anim.play("idle"));
    CHECK(anim.frame == 0);
    anim.update(0.05f);
    CHECK(anim.frame == 0);
    anim.update(0.06f); // crosses 100 ms
    CHECK(anim.frame == 1);
    CHECK(anim.frame_changed);
    anim.update(0.1f);
    CHECK(anim.frame == 0); // looped

    REQUIRE(anim.play("swing"));
    anim.update(0.1f + 0.2f + 0.1f + 0.5f); // way past the end in one big step
    CHECK(anim.finished);
    CHECK(anim.frame == 3); // holds the last frame
    CHECK(anim.play("swing")); // same animation, already finished: restarts
    CHECK(anim.frame == 1);

    REQUIRE(anim.play("bob"));
    std::vector<u32> seen{anim.frame};
    for (int i = 0; i < 5; ++i) {
        anim.update(anim.sheet->frames[anim.frame].duration);
        seen.push_back(anim.frame);
    }
    CHECK(seen == std::vector<u32>{0, 1, 2, 1, 0, 1});
}

TEST_CASE("sprite sheet: grid") {
    SpriteSheet sheet = SpriteSheet::grid({}, 16, 16, 6, 0.1f);
    CHECK(sheet.frames.size() == 6);
    CHECK(sheet.animations.size() == 1);
    CHECK(sheet.animations[0].to == 5);
}

namespace {
// Two 32x32 frames: tags with events in their user data, a hitbox slice that moves on frame 1, and a nine-slice
// panel. The first frame is trimmed (packed 16x24 at x 0, sitting at 8,8 in the full frame).
constexpr const char* sliced_json = R"({
  "frames": {
    "knight 0.aseprite": { "frame": { "x": 0, "y": 0, "w": 16, "h": 24 }, "spriteSourceSize": { "x": 8, "y": 8, "w": 16, "h": 24 },
                           "sourceSize": { "w": 32, "h": 32 }, "duration": 100 },
    "knight 1.aseprite": { "frame": { "x": 16, "y": 0, "w": 32, "h": 32 }, "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 },
                           "sourceSize": { "w": 32, "h": 32 }, "duration": 100 }
  },
  "meta": {
    "image": "knight.png",
    "frameTags": [ { "name": "walk", "from": 0, "to": 1, "direction": "forward", "data": "0:step, 1=step 1:dust bogus 9" } ],
    "slices": [
      { "name": "hitbox", "data": "hurts", "keys": [
          { "frame": 0, "bounds": { "x": 10, "y": 12, "w": 12, "h": 20 } },
          { "frame": 1, "bounds": { "x": 14, "y": 12, "w": 12, "h": 20 }, "pivot": { "x": 6, "y": 20 } } ] },
      { "name": "panel", "keys": [ { "frame": 0, "bounds": { "x": 8, "y": 8, "w": 16, "h": 24 },
          "center": { "x": 4, "y": 4, "w": 8, "h": 16 } } ] }
    ]
  }
})";
} // namespace

TEST_CASE("sprite sheet: slices, frame names and animation events") {
    SpriteSheet sheet;
    std::string image, err;
    auto doc = json::parse(sliced_json);
    REQUIRE(doc);
    REQUIRE(SpriteSheet::parse_aseprite(doc.value, sheet, image, err));

    CHECK(sheet.frame_index("knight 1.aseprite") == 1);
    CHECK(sheet.frame_index("knight 1") == 1); // without the extension
    CHECK(sheet.frame_index("nope") == -1);

    const SpriteSlice* hit = sheet.slice("hitbox");
    REQUIRE(hit);
    CHECK(hit->data == "hurts");
    CHECK(hit->at(0)->bounds.x == 10);
    CHECK(hit->at(5)->bounds.x == 14); // the last key applies from its frame on
    CHECK(hit->at(1)->pivot->x == 6);
    CHECK_FALSE(sheet.slice("missing"));

    // World rectangle: the frame's pivot (default: its center, 16,16) is drawn at the position.
    auto r = slice_rect(sheet, "hitbox", 0, {100, 100});
    REQUIRE(r);
    CHECK(r->x == doctest::Approx(94));
    CHECK(r->y == doctest::Approx(96));
    auto mirrored = slice_rect(sheet, "hitbox", 0, {100, 100}, {-1, 1});
    REQUIRE(mirrored);
    CHECK(mirrored->x == doctest::Approx(94)); // 10..22 around 16 mirrors to 10..22: a centered box stays put
    auto twice = slice_rect(sheet, "hitbox", 1, {0, 0}, {2, 2});
    REQUIRE(twice);
    CHECK(twice->w == doctest::Approx(24));

    const SpriteAnimation* walk = sheet.find("walk");
    REQUIRE(walk);
    REQUIRE(walk->events.size() == 3); // "bogus" and "9" aren't <frame>:<name>
    CHECK(walk->events[2].name == "dust");

    SpriteAnimator anim;
    anim.sheet = Asset<SpriteSheet>(std::make_shared<AssetSlot<SpriteSheet>>(AssetSlot<SpriteSheet>{
        std::make_unique<SpriteSheet>(sheet), 1, "knight.json"}));
    anim.play("walk", true); // a fresh animator is already on animation 0, so start it over explicitly
    CHECK(anim.fired("step")); // frame 0 has one
    CHECK_FALSE(anim.fired("dust"));
    anim.update(0.05f);
    CHECK_FALSE(anim.fired("step")); // still on frame 0: nothing new
    anim.update(0.06f);
    CHECK(anim.fired("step"));
    CHECK(anim.fired("dust"));
    anim.update(0.2f); // two frames at once: 0 again, then 1; both counted
    CHECK(anim.fired("step"));
}

TEST_CASE("atlas: packs, trims and writes a sheet SpriteSheet can read") {
    // Three pictures: a 4x4 with a 2x2 opaque middle, and two frames of a "walk".
    auto image = [](std::string name, u32 w, u32 h, Rect opaque) {
        atlas::Image img{std::move(name), w, h, std::vector<u8>(static_cast<usize>(w) * h * 4, 0)};
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                if (opaque.contains({static_cast<f32>(x), static_cast<f32>(y)})) img.rgba[(y * w + x) * 4 + 3] = 255;
        return img;
    };
    std::vector<atlas::Image> images = {image("dot", 4, 4, {1, 1, 2, 2}), image("walk_0", 8, 8, {0, 0, 8, 8}),
                                        image("walk_1", 8, 8, {0, 0, 8, 8})};
    std::string error;
    auto packed = atlas::pack(images, {}, error);
    REQUIRE(packed);
    const atlas::Placed& dot = packed->placed[0];
    CHECK(dot.source.w == 2);   // trimmed
    CHECK(dot.offset == Vec2{1, 1});
    CHECK(dot.size == Vec2{4, 4});
    // No two pictures overlap, and each keeps a pixel of padding.
    for (usize a = 0; a < packed->placed.size(); ++a)
        for (usize b = a + 1; b < packed->placed.size(); ++b)
            CHECK_FALSE(packed->placed[a].source.expanded(0.5f).overlaps(packed->placed[b].source));

    SpriteSheet sheet;
    std::string img, err;
    REQUIRE(SpriteSheet::parse_aseprite(atlas::sheet_json(*packed, images, "atlas.png"), sheet, img, err));
    CHECK(img == "atlas.png");
    CHECK(sheet.frame_index("dot") == 0);
    CHECK(sheet.frames[0].offset == Vec2{1, 1});
    const SpriteAnimation* walk = sheet.find("walk");
    REQUIRE(walk);
    CHECK(walk->from == 1);
    CHECK(walk->to == 2);

    CHECK_FALSE(atlas::pack(images, {.max_size = 8}, error)); // too small
    CHECK_FALSE(error.empty());
}

// Found by fuzzing (tests/fuzz): reading a tag that starts at frame 1.11111e36 was undefined behavior.
TEST_CASE("sprite sheet: frame numbers out of any range still load") {
    auto doc = json::parse(R"({ "frames": [ { "frame": { "x": 0, "y": 0, "w": 8, "h": 8 }, "duration": 100 } ],
        "meta": { "frameTags": [ { "name": "far", "from": 1.11111e36, "to": -1e36 } ],
                  "slices": [ { "name": "s", "keys": [ { "frame": 1e40, "bounds": { "x": 0, "y": 0, "w": 1, "h": 1 } } ] } ] } })");
    REQUIRE(doc);
    SpriteSheet sheet;
    std::string image, err;
    REQUIRE(SpriteSheet::parse_aseprite(doc.value, sheet, image, err));
    REQUIRE(sheet.animations.size() == 1);
    CHECK(sheet.animations[0].from == 0); // only one frame to start at
    CHECK(sheet.animations[0].to == 0);
}
