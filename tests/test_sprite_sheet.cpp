#include "ph/gfx/sprite_sheet.hpp"

#include <doctest/doctest.h>

using namespace ph;
using namespace ph::gfx;

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
