#include "bowl/core/file.hpp"
#include "bowl/core/reflect.hpp"
#include "bowl/ecs/world_io.hpp"
#include "bowl/gfx/device.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/gfx/sprite.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace bowl;
namespace fs = std::filesystem;

TEST_CASE("sprite: size falls back from size to region to a default") {
    gfx::Sprite s;
    CHECK(gfx::sprite_size(s) == Vec2{32, 32});
    s.region = {0, 0, 16, 24};
    CHECK(gfx::sprite_size(s) == Vec2{16, 24});
    s.size = {50, 10};
    CHECK(gfx::sprite_size(s) == Vec2{50, 10});
}

TEST_CASE("sprite: bounds follow the global transform, scale and origin") {
    ecs::World world;
    ecs::Entity e = world.create(ecs::Transform2D{{100, 50}, 0, {2, 1}}, gfx::Sprite{.size = {10, 20}, .origin = {0.5f, 1}});
    ecs::Entity plain = world.create(gfx::Sprite{});
    ecs::propagate_transforms(world);

    auto r = gfx::sprite_bounds(world, e);
    REQUIRE(r);
    CHECK(r->x == doctest::Approx(90));
    CHECK(r->y == doctest::Approx(30));
    CHECK(r->w == doctest::Approx(20));
    CHECK(r->h == doctest::Approx(20));
    CHECK_FALSE(gfx::sprite_bounds(world, plain)); // no transform, nothing to place it with

    world.get<ecs::Transform2D>(e).rotation = pi / 2; // a quarter turn swaps width and height
    ecs::propagate_transforms(world);
    r = gfx::sprite_bounds(world, e);
    REQUIRE(r);
    CHECK(r->w == doctest::Approx(20).epsilon(1e-3));
    CHECK(r->h == doctest::Approx(20).epsilon(1e-3));
}

TEST_CASE("sprite: saves and loads with its texture path") {
    ecs::World world;
    world.create(ecs::Transform2D{{4, 5}}, gfx::Sprite{.color = Color::hex(0xff0000), .y_sort = false});
    serial::Context ctx;
    json::Value doc = ecs::save_world(world, ctx);
    ecs::World other;
    auto spawned = ecs::load_entities(other, doc, ctx);
    REQUIRE(spawned.size() == 1);
    const gfx::Sprite& s = other.get<gfx::Sprite>(spawned[0]);
    CHECK(s.color == Color::hex(0xff0000));
    CHECK_FALSE(s.y_sort);
}

namespace {
struct Note {
    std::string text;
};
struct Holder {
    Asset<Note> note;
};
BOWL_REFLECT(Holder, note)

// Pretends the user typed a new path into an asset field.
struct RetypeAsset final : reflect::FieldVisitor {
    AssetManager* manager;
    std::string seen;
    void boolean(std::string_view, bool&) override {}
    void integer(std::string_view, i64&, i64, i64) override {}
    void real(std::string_view, f64&) override {}
    void text(std::string_view, std::string&) override {}
    void vector(std::string_view, f32*, int) override {}
    void color(std::string_view, Color&) override {}
    void entity(std::string_view, u64) override {}
    bool begin_group(std::string_view) override { return true; }
    void end_group() override {}
    void unsupported(std::string_view, std::string_view) override {}
    bool asset(std::string_view, std::string& path, std::string_view) override {
        seen = path;
        path = "b.txt";
        return true;
    }
    AssetManager* assets() override { return manager; }
};
} // namespace

TEST_CASE("reflect: asset fields can be retargeted to another path") {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / "asset_field";
    fs::remove_all(dir);
    fs::create_directories(dir);
    write_file_bytes(dir / "a.txt", "A", 1);
    write_file_bytes(dir / "b.txt", "B", 1);
    AssetManager assets;
    assets.mount("", dir);
    assets.register_loader<Note>([](AssetLoadContext& ctx) -> std::unique_ptr<Note> {
        auto t = ctx.read_text(ctx.path());
        return t ? std::make_unique<Note>(Note{*t}) : nullptr;
    });
    Holder h{assets.load<Note>("a.txt")};
    RetypeAsset v;
    v.manager = &assets;
    reflect::visit(v, "holder", h);
    CHECK(v.seen == "a.txt");
    REQUIRE(h.note);
    CHECK(h.note->text == "B");
}

TEST_CASE("world files load as assets") {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / "world_file";
    fs::remove_all(dir);
    fs::create_directories(dir);
    ecs::World world;
    world.create(ecs::Transform2D{{1, 2}});
    serial::Context ctx;
    std::string text = json::stringify(ecs::save_world(world, ctx));
    write_file_bytes(dir / "level.world", text.data(), text.size());
    write_file_bytes(dir / "bad.world", "{}", 2);

    AssetManager assets;
    assets.mount("", dir);
    ecs::register_world_file_loader(assets);
    auto level = assets.load<ecs::WorldFile>("level.world");
    REQUIRE(level);
    ecs::World other;
    CHECK(ecs::load_entities(other, level->document, ctx).size() == 1);
    CHECK_FALSE(assets.load<ecs::WorldFile>("bad.world"));
}

TEST_CASE("shader cache: stores code by key and refuses mismatches") {
    auto dir = std::filesystem::temp_directory_path() / "bowl_shader_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::vector<u8> code = {1, 2, 3, 4, 5};
    CHECK_FALSE(gfx::load_cached_shader(dir, 42));
    gfx::store_cached_shader(dir, 42, code);
    auto back = gfx::load_cached_shader(dir, 42);
    REQUIRE(back);
    CHECK(*back == code);
    CHECK_FALSE(gfx::load_cached_shader(dir, 43));
    CHECK_FALSE(gfx::load_cached_shader({}, 42)); // no cache folder: nothing cached
    std::filesystem::remove_all(dir, ec);
}

// Found by fuzzing (tests/fuzz): pictures players can swap in, decoded the way textures and window icons are.
TEST_CASE("pictures: empty, absurdly big and corrupt ones are refused safely") {
    auto data = [](const char* name) {
        auto bytes = read_file_bytes(fs::path(BOWL_TEST_DATA_DIR) / "fuzz" / name);
        REQUIRE(bytes);
        return *bytes;
    };
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    CHECK_FALSE(gfx::decode_png(data("bmp_zero_height.bmp"), w, h, rgba)); // 0 pixels high: no texture to make
    // 25 bytes whose header claims about 24000 x 23000 pixels: refused instead of allocating gigabytes.
    CHECK_FALSE(gfx::decode_png(data("tga_huge_header.tga"), w, h, rgba));
    // Coefficients that overflow int in the JPEG decoder (undefined behavior before stb_image.cpp got -fwrapv).
    if (gfx::decode_png(data("jpeg_idct_overflow.jpg"), w, h, rgba)) CHECK(rgba.size() == usize{w} * h * 4);
}
