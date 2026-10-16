#include "bowl/core/math.hpp"
#include "bowl/core/shader_bundle.hpp"
#include "bowl/core/type_id.hpp"

#include <doctest/doctest.h>

using namespace bowl;

namespace demo {
struct Thing {};
} // namespace demo

TEST_CASE("math: vectors") {
    Vec2 a{3, 4};
    CHECK(a.length() == doctest::Approx(5));
    CHECK(a.normalized().length() == doctest::Approx(1));
    CHECK((a + Vec2{1, 1}) == Vec2{4, 5});
    Vec2 r = Vec2{1, 0}.rotated(pi / 2);
    CHECK(r.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(r.y == doctest::Approx(1));
    CHECK(Vec3{1, 0, 0}.cross({0, 1, 0}) == Vec3{0, 0, 1});
}

TEST_CASE("math: orthographic projection maps the view rectangle to clip space") {
    // y-down screen: top (y=0) -> +1, bottom (y=720) -> -1
    Mat4 p = Mat4::ortho(0, 1280, 720, 0);
    Vec2 top_left = p.transform_point({0, 0});
    Vec2 bottom_right = p.transform_point({1280, 720});
    CHECK(top_left.x == doctest::Approx(-1));
    CHECK(top_left.y == doctest::Approx(1));
    CHECK(bottom_right.x == doctest::Approx(1));
    CHECK(bottom_right.y == doctest::Approx(-1));
}

TEST_CASE("math: matrix multiply composes transforms") {
    Mat4 m = Mat4::translation({10, 0, 0}) * Mat4::scale({2, 2, 1});
    Vec2 p = m.transform_point({1, 1});
    CHECK(p.x == doctest::Approx(12));
    CHECK(p.y == doctest::Approx(2));
}

TEST_CASE("type names are readable and stable") {
    CHECK(type_name<demo::Thing>() == "demo::Thing");
    CHECK(type_name<int>() == "int");
    static_assert(type_hash<demo::Thing>() == fnv1a("demo::Thing"));
}

TEST_CASE("shader bundle round-trips") {
    ShaderBundle b;
    b.stage = ShaderStage::Fragment;
    b.samplers = 2;
    b.uniform_buffers = 1;
    b.blobs.push_back({ShaderBlobFormat::SpirV, "main", {1, 2, 3, 4}});
    b.blobs.push_back({ShaderBlobFormat::Msl, "main0", {'x', 'y'}});
    auto bytes = b.serialize();
    auto parsed = ShaderBundle::parse(bytes);
    REQUIRE(parsed);
    CHECK(parsed->stage == ShaderStage::Fragment);
    CHECK(parsed->samplers == 2);
    CHECK(parsed->uniform_buffers == 1);
    REQUIRE(parsed->find(ShaderBlobFormat::Msl));
    CHECK(parsed->find(ShaderBlobFormat::Msl)->entry_point == "main0");
    bytes.resize(bytes.size() - 1);
    CHECK_FALSE(ShaderBundle::parse(bytes)); // truncated
}
