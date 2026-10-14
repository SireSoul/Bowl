// Isometric: a y-sorted isometric map with a walking character.
//
//   WASD / left stick   walk (in screen directions)
//   mouse wheel         zoom
//   F1                  dev overlay
//
// The map is built in code to keep the sample self-contained; games load Tiled maps with
// assets.load<gfx::Tilemap>("maps/town.tmj") and draw them the same way.

#include "ph/app/main.hpp"
#include "ph/gfx/text.hpp"
#include "ph/gfx/tilemap.hpp"
#include "ph/physics/collision.hpp"

#include <cmath>

using namespace ph;
using namespace ph::ecs;

namespace {

constexpr u32 tile_w = 64, tile_h = 32, image_h = 96, map_size = 24;
enum Tile : u32 { Grass = 1, Water = 2, Stone = 3, Pillar = 4, Water2 = 5 };

struct Player {
    Vec2 cell{map_size / 2.0f, map_size / 2.0f}; // position in (fractional) map cells
    Vec2 previous = cell;
};

f32 hash01(u32 x, u32 y) {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>(h & 0xFFFF) / 65535.0f;
}

} // namespace

class IsometricGame final : public Game {
public:
    AppConfig configure() override {
        AppConfig config;
        config.window.title = "PLACEHOLDERENGINENAME — isometric";
        return config;
    }

    void init(Engine& engine) override {
        font_ = engine.assets().load<gfx::Font>("engine/fonts/Roboto-Medium.ttf");
        Input& in = engine.input();
        in.bind("left", {InputBinding::key(Key::A), InputBinding::key(Key::Left), InputBinding::axis(GamepadAxis::LeftX, -1)});
        in.bind("right", {InputBinding::key(Key::D), InputBinding::key(Key::Right), InputBinding::axis(GamepadAxis::LeftX, +1)});
        in.bind("up", {InputBinding::key(Key::W), InputBinding::key(Key::Up), InputBinding::axis(GamepadAxis::LeftY, -1)});
        in.bind("down", {InputBinding::key(Key::S), InputBinding::key(Key::Down), InputBinding::axis(GamepadAxis::LeftY, +1)});

        build_map(engine);
        // --behind-pillar: start just behind a pillar (to check that the pillar hides the player).
        if (engine.has_arg("--behind-pillar")) {
            const gfx::MapLayer& objects = map_->layers[1];
            for (i32 y = 1; y < static_cast<i32>(map_size); ++y)
                for (i32 x = 0; x < static_cast<i32>(map_size); ++x)
                    if ((objects.at(x, y) & gfx::tile_gid_mask) == Pillar && objects.at(x - 1, y - 1) == 0 &&
                        (map_->layers[0].at(x - 1, y - 1) & gfx::tile_gid_mask) != Water) {
                        player_.cell = player_.previous = {static_cast<f32>(x) - 0.5f, static_cast<f32>(y) - 0.5f};
                        y = static_cast<i32>(map_size);
                        break;
                    }
        }
        player_tex_ = engine.assets().add("generated/player", make_player(engine.gpu()));
        engine.renderer().set_clear_color(Color::hex(0x0d1b2a));
        engine.camera().zoom = 1.2f;
    }

    void fixed_update(Engine& engine, f32 dt) override {
        Player& p = player_;
        p.previous = p.cell;
        // Screen-relative movement: convert the screen direction into map axes (x - y is right, x + y is down).
        Vec2 dir = engine.input().vector("left", "right", "up", "down");
        Vec2 grid{dir.x + dir.y * 2.0f, dir.y * 2.0f - dir.x};
        if (grid.length() > 0) grid = grid.normalized() * (dir.length() * 4.0f * dt);
        // Collide in map-cell space: the player is a small box, water/pillar/off-map cells are solid.
        auto solid = [&](i32 x, i32 y) {
            if (x < 0 || y < 0 || x >= static_cast<i32>(map_size) || y >= static_cast<i32>(map_size)) return true;
            return (map_->layers[0].at(x, y) & gfx::tile_gid_mask) == Water ||
                   (map_->layers[1].at(x, y) & gfx::tile_gid_mask) == Pillar;
        };
        constexpr f32 r = 0.2f;
        Rect box{p.cell.x - r, p.cell.y - r, 2 * r, 2 * r};
        physics::move_and_slide(box, grid, physics::GridSolids{{1, 1}, {0, 0}, solid});
        p.cell = box.center();
    }

    void update(Engine& engine, f32) override {
        gfx::Camera2D& cam = engine.camera();
        if (engine.input().wheel() != 0) cam.zoom = clamp(cam.zoom * std::pow(1.1f, engine.input().wheel()), 0.4f, 4.0f);
        Vec2 target = map_->cell_to_world(player_cell(engine).x, player_cell(engine).y);
        cam.position = lerp(cam.position, target, 0.15f); // smooth follow
    }

    void render(Engine& engine) override {
        batch_.begin(engine.camera());
        gfx::draw_tilemap(batch_, *map_, {.time = static_cast<f32>(engine.time()), .y_sort = true});
        // The player's depth is its feet's y, so it sorts against the pillars' bases.
        Vec2 feet = map_->cell_to_world(player_cell(engine).x, player_cell(engine).y);
        gfx::SpriteInstance s;
        s.position = feet;
        s.size = player_tex_->size();
        s.origin = {0.5f, 0.92f};
        s.depth = feet.y + 0.5f;
        batch_.draw(*player_tex_, s);
        engine.renderer().submit(batch_);

        if (!font_) return;
        ui_.begin(gfx::Camera2D::screen(engine.window().size()), gfx::SpriteSort::None);
        gfx::TextStyle style{.size = 18, .raster_scale = engine.window().pixel_density()};
        Vec2 c = player_cell(engine);
        gfx::draw_text(ui_, *font_, std::format("Isometric · cell {:.0f}, {:.0f} · WASD to walk, wheel to zoom", c.x, c.y),
                       {20, 18}, style);
        engine.renderer().submit(ui_);
    }

private:
    Vec2 player_cell(Engine& engine) const { return lerp(player_.previous, player_.cell, engine.alpha()); }

    void build_map(Engine& engine) {
        auto map = std::make_unique<gfx::Tilemap>();
        map->orientation = gfx::Tilemap::Orientation::Isometric;
        map->width = map->height = map_size;
        map->tile_width = tile_w;
        map->tile_height = tile_h;

        gfx::Tileset ts;
        ts.name = "generated";
        ts.tile_width = tile_w;
        ts.tile_height = image_h;
        ts.columns = 5;
        ts.tile_count = 5;
        ts.image = engine.assets().add("generated/iso_tiles", make_tiles(engine.gpu()));
        gfx::TileInfo water; // animated: two frames of water
        water.id = Water - 1;
        water.animation = {{Water - 1, 0.6f}, {Water2 - 1, 0.6f}};
        ts.tiles.push_back(water);
        map->tilesets.push_back(std::move(ts));

        gfx::MapLayer ground, objects;
        ground.name = "ground";
        objects.name = "objects";
        for (gfx::MapLayer* l : {&ground, &objects}) {
            l->width = l->height = map_size;
            l->cells.assign(map_size * map_size, 0);
        }
        Vec2 center{map_size / 2.0f, map_size / 2.0f};
        for (u32 y = 0; y < map_size; ++y)
            for (u32 x = 0; x < map_size; ++x) {
                f32 d = (Vec2{static_cast<f32>(x), static_cast<f32>(y)} - center).length();
                f32 n = hash01(x, y);
                u32 tile = d > 10.5f + n * 1.5f ? Water : (n < 0.12f ? Stone : Grass);
                ground.cells[y * map_size + x] = tile;
                if (tile != Water && d > 2.5f && hash01(x * 7 + 3, y * 13 + 1) < 0.07f) objects.cells[y * map_size + x] = Pillar;
            }
        map->layers.push_back(std::move(ground));
        map->layers.push_back(std::move(objects));
        map_ = engine.assets().add("generated/island", std::move(map));
    }

    // Five 64x96 tile images in a strip: grass, water, stone, pillar, water (2nd frame). The diamond sits in the
    // bottom 32 pixels; the pillar rises above it.
    static std::unique_ptr<gfx::Texture> make_tiles(gfx::GpuDevice& gpu) {
        constexpr u32 count = 5, w = tile_w * count;
        std::vector<u8> px(w * image_h * 4, 0);
        auto put = [&](u32 tile, u32 x, u32 y, Color c) {
            u8* p = &px[(y * w + tile * tile_w + x) * 4];
            p[0] = static_cast<u8>(saturate(c.r) * 255);
            p[1] = static_cast<u8>(saturate(c.g) * 255);
            p[2] = static_cast<u8>(saturate(c.b) * 255);
            p[3] = static_cast<u8>(saturate(c.a) * 255);
        };
        auto in_diamond = [](f32 x, f32 y) { // x in 0..64, y in 0..32 (relative to the diamond's box)
            return std::abs(x - 32.0f) / 32.0f + std::abs(y - 16.0f) / 16.0f <= 1.0f;
        };
        for (u32 t = 0; t < count; ++t)
            for (u32 y = 0; y < image_h; ++y)
                for (u32 x = 0; x < tile_w; ++x) {
                    f32 fx = static_cast<f32>(x) + 0.5f, fy = static_cast<f32>(y) + 0.5f;
                    f32 dy = fy - (image_h - tile_h); // y within the bottom diamond box
                    f32 n = hash01(x + t * 64, y);
                    if (t == Pillar - 1) {
                        // A stone column: top cap diamond, two shaded faces down to the base diamond.
                        bool body = std::abs(fx - 32) < 18 && fy > 22 && dy < 16 + 9.0f * (1 - std::abs(fx - 32) / 18);
                        bool cap = std::abs(fx - 32) / 18 + std::abs(fy - 22) / 9 <= 1;
                        if (cap) put(t, x, y, Color::hex(0xc9c2b8));
                        else if (body) put(t, x, y, fx < 32 ? Color::hex(0x8f877c) : Color::hex(0x6e675e));
                        continue;
                    }
                    if (dy < 0 || !in_diamond(fx, dy)) continue;
                    Color c = t == Grass - 1   ? lerp(Color::hex(0x4f9a4a), Color::hex(0x6cb85a), n)
                              : t == Stone - 1 ? lerp(Color::hex(0x8a8f96), Color::hex(0xa3a8ae), n)
                                               : Color::hex(0x2f6fa8);
                    if (t == Water - 1 || t == Water2 - 1) {
                        f32 phase = t == Water - 1 ? 0.0f : 3.14f;
                        f32 wave = 0.5f + 0.5f * std::sin(fx * 0.35f + dy * 0.7f + phase);
                        c = lerp(Color::hex(0x2b64a0), Color::hex(0x4f95d6), wave * 0.8f);
                    }
                    // A soft shade along the lower edges so tiles read as slightly raised.
                    if (dy > 16 && std::abs(fx - 32.0f) / 32.0f + std::abs(dy - 16.0f) / 16.0f > 0.92f)
                        c = lerp(c, Color::black(), 0.12f);
                    put(t, x, y, c);
                }
        return gpu.create_texture({.width = w, .height = image_h, .rgba8 = px.data(), .name = "iso tiles"});
    }

    static std::unique_ptr<gfx::Texture> make_player(gfx::GpuDevice& gpu) {
        constexpr u32 w = 28, h = 44;
        std::vector<u8> px(w * h * 4, 0);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                f32 fx = (static_cast<f32>(x) + 0.5f - w / 2.0f) / (w / 2.0f), fy = static_cast<f32>(y) + 0.5f;
                f32 a = 0;
                Color c = Color::hex(0xffcf5c);
                if (fy < 18) a = saturate((1 - std::sqrt(fx * fx * 2.4f + ((fy - 9) / 9) * ((fy - 9) / 9))) * 8); // head
                else if (fy < 40) { a = saturate((0.8f - std::abs(fx)) * 8); c = Color::hex(0xe8556d); } // body
                else { a = saturate((1 - std::abs(fx)) * 4) * 0.35f; c = Color::black(); } // shadow
                u8* p = &px[(y * w + x) * 4];
                p[0] = static_cast<u8>(c.r * 255);
                p[1] = static_cast<u8>(c.g * 255);
                p[2] = static_cast<u8>(c.b * 255);
                p[3] = static_cast<u8>(a * 255);
            }
        return gpu.create_texture({.width = w, .height = h, .rgba8 = px.data(), .name = "player"});
    }

    Asset<gfx::Tilemap> map_;
    Asset<gfx::Texture> player_tex_;
    Asset<gfx::Font> font_;
    gfx::SpriteBatch batch_, ui_;
    Player player_;
};

PH_MAIN(IsometricGame)
