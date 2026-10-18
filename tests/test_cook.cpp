#include "bowl/cook/cook.hpp"

#include "bowl/core/assets.hpp"
#include "bowl/core/file.hpp"
#include "bowl/core/json.hpp"
#include "bowl/core/localization.hpp"
#include "bowl/ecs/world_io.hpp"
#include "bowl/gfx/bwtx.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>

using namespace bowl;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& root, const std::string& path, std::string_view text) {
    fs::create_directories((root / path).parent_path());
    REQUIRE(write_file_bytes(root / path, text.data(), text.size()));
}

std::vector<u8> bytes_of(const fs::path& file) {
    auto bytes = read_file_bytes(file);
    REQUIRE(bytes);
    return *bytes;
}

std::map<std::string, std::vector<u8>> folder_contents(const fs::path& root) {
    std::map<std::string, std::vector<u8>> out;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) out[fs::relative(e.path(), root).generic_string()] = bytes_of(e.path());
    return out;
}

// A picture with something for every kind of step in the format: long runs, repeats of earlier colours, small and
// medium changes (some wrapping past 255), big jumps and changing alpha.
std::vector<u8> test_pixels(u32 w, u32 h) {
    std::vector<u8> rgba(usize{w} * h * 4);
    u32 seed = 12345;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = rgba.data() + (usize{y} * w + x) * 4;
            seed = seed * 1664525u + 1013904223u;
            u32 band = y / 8 % 5;
            if (band == 0) { // runs longer than 62
                p[0] = 10, p[1] = 20, p[2] = 30, p[3] = 255;
            } else if (band == 1) { // a gradient: small and medium differences, wrapping
                p[0] = static_cast<u8>(250 + x), p[1] = static_cast<u8>(x * 3), p[2] = static_cast<u8>(x * 7 + y), p[3] = 255;
            } else if (band == 2) { // a few colours over and over
                static const u8 palette[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 128}, {9, 9, 9, 0}};
                std::copy(palette[(x / 3) % 4], palette[(x / 3) % 4] + 4, p);
            } else if (band == 3) { // noise
                p[0] = static_cast<u8>(seed >> 24), p[1] = static_cast<u8>(seed >> 16), p[2] = static_cast<u8>(seed >> 8),
                p[3] = 255;
            } else { // alpha changing
                p[0] = 100, p[1] = 50, p[2] = 25, p[3] = static_cast<u8>(x * 5);
            }
        }
    return rgba;
}

// The tilemap tests' tileset and template, in folders of their own as Tiled keeps them.
constexpr const char* terrain_tsj = R"({
  "name": "terrain", "tilewidth": 16, "tileheight": 16, "columns": 4, "tilecount": 8, "margin": 1, "spacing": 2,
  "image": "../art/terrain.png", "imagewidth": 72, "imageheight": 38, "type": "tileset", "version": "1.10",
  "tiles": [
    { "id": 1, "properties": [ { "name": "solid", "type": "bool", "value": true } ] },
    { "id": 4, "image": "../art/tree.png", "imagewidth": 32, "imageheight": 48 },
    { "id": 5, "animation": [ { "tileid": 5, "duration": 100 }, { "tileid": 6, "duration": 100 } ] }
  ]
})";

constexpr const char* props_tsx = R"(<?xml version="1.0" encoding="UTF-8"?>
<tileset name="props" tilewidth="16" tileheight="16" tilecount="4" columns="2">
 <image source="props.png" width="32" height="32"/>
 <tile id="2"><properties><property name="loot" value="coins"/></properties></tile>
</tileset>
)";

constexpr const char* chest_tj = R"({ "type": "template", "tileset": { "firstgid": 1, "source": "../tilesets/terrain.tsj" },
  "object": { "gid": 3, "width": 16, "height": 16, "name": "chest", "type": "loot",
              "properties": [ { "name": "gold", "type": "int", "value": 5 } ] } })";

constexpr const char* zone_tx = R"(<?xml version="1.0" encoding="UTF-8"?>
<template>
 <object name="zone" type="spawn" width="32" height="16"/>
</template>)";

// Layer data as Tiled's base64 (no compression): little-endian u32s.
std::string base64_gids(const std::vector<u32>& gids) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<u8> raw;
    for (u32 g : gids)
        for (int i = 0; i < 4; ++i) raw.push_back(static_cast<u8>(g >> (8 * i)));
    std::string out;
    u32 buffer = 0;
    int bits = 0;
    for (u8 b : raw) {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += table[(buffer >> bits) & 63];
        }
    }
    if (bits > 0) out += table[(buffer << (6 - bits)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}

// A small game's assets with one of everything cooking changes.
void write_game(const fs::path& root) {
    put(root, "tilesets/terrain.tsj", terrain_tsj);
    put(root, "maps/props.tsx", props_tsx);
    put(root, "templates/chest.tj", chest_tj);
    put(root, "templates/zone.tx", zone_tx);
    put(root, "tilesets/unused.tsj", terrain_tsj); // no map names it: copied
    put(root, "maps/town.tmj", std::format(R"({{ "orientation": "isometric", "width": 3, "height": 2,
      "tilewidth": 32, "tileheight": 16, "properties": [ {{ "name": "music", "type": "string", "value": "town.ogg" }} ],
      "tilesets": [ {{ "firstgid": 1, "source": "../tilesets/terrain.tsj" }}, {{ "firstgid": 20, "source": "props.tsx" }} ],
      "layers": [
        {{ "type": "tilelayer", "name": "ground", "width": 3, "height": 2, "data": [1, 2, 3, 2147483649, 0, 6] }},
        {{ "type": "group", "name": "top", "offsetx": 4, "layers": [
          {{ "type": "tilelayer", "name": "roofs", "width": 3, "height": 2, "encoding": "base64",
             "data": "{}", "properties": [ {{ "name": "above", "type": "bool", "value": true }} ] }} ] }},
        {{ "type": "objectgroup", "name": "things", "objects": [
          {{ "id": 1, "template": "../templates/chest.tj", "x": 10, "y": 20,
             "properties": [ {{ "name": "gold", "type": "int", "value": 50 }} ] }},
          {{ "id": 2, "template": "../templates/zone.tx", "x": 64, "y": 0 }},
          {{ "id": 3, "x": 1, "y": 2, "polygon": [ {{ "x": 0, "y": 0 }}, {{ "x": 5, "y": 0 }}, {{ "x": 0, "y": 5 }} ] }} ] }},
        {{ "type": "imagelayer", "name": "sky", "image": "../art/sky.png" }} ] }})",
                                           base64_gids({0, 21, 22, 0, 0, 23})));
    put(root, "maps/cave.tmx", R"(<?xml version="1.0" encoding="UTF-8"?>
<map orientation="orthogonal" width="2" height="2" tilewidth="16" tileheight="16" infinite="1">
 <tileset firstgid="1" source="../tilesets/terrain.tsj"/>
 <layer id="1" name="floor" width="2" height="2">
  <data encoding="csv">
   <chunk x="-16" y="0" width="2" height="1">1,2</chunk>
   <chunk x="0" y="16" width="1" height="2">3,
4</chunk>
  </data>
 </layer>
</map>
)");
    put(root, "levels/start.world", R"({ "format": "Bowl.world", "version": 1, "entities": [
      // comments and trailing commas, as hand-written files have them
      { "id": 0, "components": { "bowl::ecs::Transform2D": { "position": [32, 16], "rotation": 0.5 },
                                 "bowl::ecs::Name": { "value": "hero" }, } } ] })");
    put(root, "props/barrel.prefab", R"({ "components": { "bowl::ecs::Name": { "value": "barrel" } } })");
    put(root, "lang/en.json", R"({ "menu": { "start": "Start" }, "coins": { "one": "{n} coin", "other": "{n} coins" } })");
    put(root, "data/items.json", R"({ "sword": 3 })"); // the game's own JSON: copied
    put(root, "sounds/hit.ogg", "not really a sound");
    u32 w = 70, h = 45;
    REQUIRE(gfx::write_png(root / "art/terrain.png", w, h, test_pixels(w, h)));
}

void check_same(const gfx::Tilemap& a, const gfx::Tilemap& b) {
    CHECK(a.orientation == b.orientation);
    CHECK(a.width == b.width);
    CHECK(a.height == b.height);
    CHECK(a.tile_width == b.tile_width);
    CHECK(a.tile_height == b.tile_height);
    CHECK(a.infinite == b.infinite);
    CHECK(a.properties == b.properties);
    REQUIRE(a.tilesets.size() == b.tilesets.size());
    for (usize i = 0; i < a.tilesets.size(); ++i) {
        const gfx::Tileset &x = a.tilesets[i], &y = b.tilesets[i];
        CHECK(x.name == y.name);
        CHECK(x.first_gid == y.first_gid);
        CHECK(x.columns == y.columns);
        CHECK(x.tile_count == y.tile_count);
        CHECK(x.margin == y.margin);
        CHECK(x.spacing == y.spacing);
        CHECK(x.image_path == y.image_path);
        CHECK(x.source == y.source);
        CHECK(x.properties == y.properties);
        REQUIRE(x.tiles.size() == y.tiles.size());
        for (usize t = 0; t < x.tiles.size(); ++t) {
            CHECK(x.tiles[t].id == y.tiles[t].id);
            CHECK(x.tiles[t].properties == y.tiles[t].properties);
            CHECK(x.tiles[t].image_path == y.tiles[t].image_path);
            CHECK(x.tiles[t].animation.size() == y.tiles[t].animation.size());
        }
    }
    REQUIRE(a.layers.size() == b.layers.size());
    for (usize i = 0; i < a.layers.size(); ++i) {
        const gfx::MapLayer &x = a.layers[i], &y = b.layers[i];
        CHECK(x.kind == y.kind);
        CHECK(x.name == y.name);
        CHECK(x.offset == y.offset);
        CHECK(x.properties == y.properties);
        CHECK(x.origin_x == y.origin_x);
        CHECK(x.origin_y == y.origin_y);
        CHECK(x.width == y.width);
        CHECK(x.height == y.height);
        CHECK(x.cells == y.cells);
        CHECK(x.image_path == y.image_path);
        REQUIRE(x.objects.size() == y.objects.size());
        for (usize o = 0; o < x.objects.size(); ++o) {
            const gfx::MapObject &p = x.objects[o], &q = y.objects[o];
            CHECK(p.id == q.id);
            CHECK(p.name == q.name);
            CHECK(p.type == q.type);
            CHECK(p.shape == q.shape);
            CHECK(p.bounds == q.bounds);
            CHECK(p.gid == q.gid);
            CHECK(p.points == q.points);
            CHECK(p.properties == q.properties);
        }
    }
}

// The map as the engine reads its source: tilesets and templates from the folder.
gfx::Tilemap load_source_map(const fs::path& root, const std::string& path) {
    auto read = [&](const std::string& p) { return read_file_text(root / p); };
    auto text = read_file_text(root / path);
    REQUIRE(text);
    gfx::Tilemap map;
    std::string error;
    std::string dir = path.substr(0, path.rfind('/') + 1);
    bool ok = path.ends_with(".tmx") ? gfx::Tilemap::parse_tmx(*text, dir, read, map, error)
                                     : gfx::Tilemap::parse(json::parse(*text).value, dir, read, map, error);
    REQUIRE_MESSAGE(ok, error);
    return map;
}

} // namespace

TEST_CASE("bwtx: pictures come back pixel for pixel") {
    for (auto [w, h] : {std::pair<u32, u32>{1, 1}, {70, 45}, {200, 3}, {3, 200}}) {
        std::vector<u8> rgba = test_pixels(w, h);
        std::vector<u8> bwtx = gfx::encode_bwtx(w, h, rgba);
        CHECK(gfx::is_bwtx(bwtx));
        u32 dw = 0, dh = 0;
        std::vector<u8> back;
        REQUIRE(gfx::decode_bwtx(bwtx, dw, dh, back));
        CHECK(dw == w);
        CHECK(dh == h);
        CHECK(back == rgba);
    }
    // One colour packs into runs: 10000 pixels in a few hundred bytes.
    std::vector<u8> flat(100 * 100 * 4, 7);
    CHECK(gfx::encode_bwtx(100, 100, flat).size() < 200);
}

TEST_CASE("bwtx: a cooked PNG decodes to the same pixels as the PNG") {
    fs::path dir = fresh_dir("cook_png");
    u32 w = 70, h = 45;
    REQUIRE(gfx::write_png(dir / "a.png", w, h, test_pixels(w, h)));
    std::vector<u8> png = bytes_of(dir / "a.png");
    std::string error;
    auto cooked = cook::cook_file("art/a.png", png, {}, error);
    REQUIRE_MESSAGE(cooked, error);
    CHECK(gfx::is_bwtx(cooked->bytes));
    u32 pw = 0, ph = 0, cw = 0, ch = 0;
    std::vector<u8> from_png, from_cooked;
    REQUIRE(gfx::decode_png(png, pw, ph, from_png));
    REQUIRE(gfx::decode_png(cooked->bytes, cw, ch, from_cooked)); // what the texture loader calls
    CHECK(cw == pw);
    CHECK(ch == ph);
    CHECK(from_cooked == from_png);
    // Cooked already: left as it is.
    CHECK_FALSE(cook::cook_file("art/a.png", cooked->bytes, {}, error));
    CHECK(error.empty());
    // Not a picture at all.
    std::string junk = "not a png";
    CHECK_FALSE(cook::cook_file("art/b.png", std::span(reinterpret_cast<const u8*>(junk.data()), junk.size()), {}, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("bwtx: broken pictures fail, without asking for memory they don't hold") {
    std::vector<u8> good = gfx::encode_bwtx(20, 10, test_pixels(20, 10));
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    for (usize cut : {usize{0}, usize{8}, usize{16}, good.size() / 2, good.size() - 9}) {
        std::vector<u8> part(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(gfx::decode_bwtx(part, w, h, rgba));
    }
    std::vector<u8> huge = good;
    huge[8] = huge[9] = huge[12] = huge[13] = 0xFF; // 65535 x 65535: more than a texture can be
    huge[10] = huge[14] = 0;
    CHECK_FALSE(gfx::decode_bwtx(huge, w, h, rgba));
    std::vector<u8> big = good;
    big[9] = big[13] = 0x40; // 16384 x 16384 from a few hundred bytes
    big[8] = big[12] = 0;
    CHECK_FALSE(gfx::decode_bwtx(big, w, h, rgba));
    std::vector<u8> version = good;
    version[4] = 2;
    CHECK_FALSE(gfx::decode_bwtx(version, w, h, rgba));
    // A run past the last pixel.
    std::vector<u8> runs{'B', 'W', 'T', 'X', 1, 4, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0xC0 | 61, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK_FALSE(gfx::decode_bwtx(runs, w, h, rgba));
    runs[16] = 0xC0 | 1;
    REQUIRE(gfx::decode_bwtx(runs, w, h, rgba));
    CHECK(rgba == std::vector<u8>{0, 0, 0, 255, 0, 0, 0, 255});
}

TEST_CASE("cook: files are cooked by their names") {
    CHECK(cook::kind_of("levels/start.world") == cook::Kind::Json);
    CHECK(cook::kind_of("a.prefab") == cook::Kind::Json);
    CHECK(cook::kind_of("lang/en.json") == cook::Kind::Json);
    CHECK(cook::kind_of("data/lang/en.json") == cook::Kind::Copy);
    CHECK(cook::kind_of("lang/old/en.json") == cook::Kind::Copy);
    CHECK(cook::kind_of("items.json") == cook::Kind::Copy);
    CHECK(cook::kind_of("maps/town.tmj") == cook::Kind::Map);
    CHECK(cook::kind_of("maps/cave.tmx") == cook::Kind::Map);
    CHECK(cook::kind_of("maps/terrain.tsj") == cook::Kind::Copy);
    CHECK(cook::kind_of("art/Hero.PNG") == cook::Kind::Picture);
    CHECK(cook::kind_of("art/sky.jpeg") == cook::Kind::Picture);
    CHECK(cook::kind_of("fonts/a.ttf") == cook::Kind::Copy);
    CHECK(cook::kind_of("png/readme") == cook::Kind::Copy);
}

TEST_CASE("cook: a cooked game's worlds, maps and strings load the same as the source") {
    fs::path src = fresh_dir("cook_src"), out = fresh_dir("cook_out");
    write_game(src);
    cook::Report report;
    REQUIRE_MESSAGE(cook::cook_folder(src, out, report), (report.errors.empty() ? "" : report.errors[0]));
    CHECK(report.left_out == 4); // terrain.tsj, props.tsx, chest.tj, zone.tx
    CHECK(report.cooked == 6);   // 2 maps, world, prefab, strings, picture

    // The files put into maps are gone; everything else is where it was.
    CHECK_FALSE(fs::exists(out / "tilesets/terrain.tsj"));
    CHECK_FALSE(fs::exists(out / "maps/props.tsx"));
    CHECK_FALSE(fs::exists(out / "templates/chest.tj"));
    CHECK_FALSE(fs::exists(out / "templates/zone.tx"));
    CHECK(bytes_of(out / "tilesets/unused.tsj") == bytes_of(src / "tilesets/unused.tsj"));
    CHECK(bytes_of(out / "data/items.json") == bytes_of(src / "data/items.json"));
    CHECK(bytes_of(out / "sounds/hit.ogg") == bytes_of(src / "sounds/hit.ogg"));
    CHECK(gfx::is_bwtx(bytes_of(out / "art/terrain.png")));

    // Maps: the cooked one is binary JSON and reads with no other file.
    for (const char* path : {"maps/town.tmj", "maps/cave.tmx"}) {
        CAPTURE(path);
        gfx::Tilemap source = load_source_map(src, path);
        std::vector<u8> bytes = bytes_of(out / path);
        REQUIRE(json::is_binary(bytes));
        auto doc = json::parse_any(bytes);
        REQUIRE(doc);
        gfx::Tilemap cooked;
        std::string error;
        auto nothing = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
        REQUIRE_MESSAGE(gfx::Tilemap::parse(doc.value, "maps/", nothing, cooked, error), error);
        check_same(source, cooked);
    }
    gfx::Tilemap town = load_source_map(src, "maps/town.tmj");
    CHECK(town.tilesets[0].source == "tilesets/terrain.tsj");
    CHECK(town.tilesets[0].image_path == "art/terrain.png");
    CHECK(town.tilesets[0].tile(4)->image_path == "art/tree.png");
    CHECK(town.layer("top/roofs")->cells == std::vector<u32>{0, 21, 22, 0, 0, 23});
    CHECK(town.layer("things")->objects[0].gid == 3);

    // Through the asset manager, as a game loads them.
    AssetManager source_assets, cooked_assets;
    source_assets.mount("", src);
    cooked_assets.mount("", out);
    for (AssetManager* a : {&source_assets, &cooked_assets}) {
        gfx::register_tilemap_loader(*a);
        ecs::register_world_file_loader(*a);
        register_string_table_loader(*a);
    }
    for (const char* path : {"maps/town.tmj", "maps/cave.tmx"}) {
        auto a = source_assets.load<gfx::Tilemap>(path);
        auto b = cooked_assets.load<gfx::Tilemap>(path);
        REQUIRE(a);
        REQUIRE(b);
        check_same(*a, *b);
    }
    auto world_a = source_assets.load<ecs::WorldFile>("levels/start.world");
    auto world_b = cooked_assets.load<ecs::WorldFile>("levels/start.world");
    REQUIRE(world_a);
    REQUIRE(world_b);
    CHECK(world_a->document == world_b->document);
    ecs::World wa, wb;
    serial::Context ca, cb;
    auto ea = ecs::load_entities(wa, world_a->document, ca), eb = ecs::load_entities(wb, world_b->document, cb);
    REQUIRE(ea.size() == 1);
    REQUIRE(eb.size() == 1);
    CHECK(wa.get<ecs::Transform2D>(ea[0]).position == wb.get<ecs::Transform2D>(eb[0]).position);
    CHECK(wa.get<ecs::Transform2D>(ea[0]).rotation == wb.get<ecs::Transform2D>(eb[0]).rotation);
    CHECK(wb.get<ecs::Name>(eb[0]).value == "hero");
    auto prefab_a = source_assets.load<ecs::Prefab>("props/barrel.prefab");
    auto prefab_b = cooked_assets.load<ecs::Prefab>("props/barrel.prefab");
    REQUIRE(prefab_a);
    REQUIRE(prefab_b);
    CHECK(prefab_a->document == prefab_b->document);
    auto strings_a = source_assets.load<StringTable>("lang/en.json");
    auto strings_b = cooked_assets.load<StringTable>("lang/en.json");
    REQUIRE(strings_a);
    REQUIRE(strings_b);
    CHECK(strings_a->strings == strings_b->strings);
    CHECK(*strings_b->find("coins.other") == "{n} coins");
}

TEST_CASE("cook: the same folder always cooks to the same bytes, and cooking it again changes nothing") {
    fs::path src = fresh_dir("cook_same_src"), a = fresh_dir("cook_same_a"), b = fresh_dir("cook_same_b");
    write_game(src);
    cook::Report ra, rb, rc, rd;
    REQUIRE(cook::cook_folder(src, a, ra));
    REQUIRE(cook::cook_folder(src, b, rb));
    auto first = folder_contents(a);
    CHECK(first == folder_contents(b));
    // In place, as the build does it: a cooked folder stays as it is.
    REQUIRE(cook::cook_folder(a, a, rc));
    CHECK(rc.cooked == 0);
    CHECK(folder_contents(a) == first);
    // In place from the source: the same as cooking into another folder.
    REQUIRE(cook::cook_folder(src, src, rd));
    CHECK(folder_contents(src) == first);
}

TEST_CASE("cook: a broken file fails the cook, and maps keep what they need") {
    fs::path src = fresh_dir("cook_broken_src"), out = fresh_dir("cook_broken_out");
    put(src, "tilesets/terrain.tsj", terrain_tsj);
    put(src, "maps/good.tmj", R"({ "orientation": "orthogonal", "width": 1, "height": 1, "tilewidth": 16,
      "tileheight": 16, "tilesets": [ { "firstgid": 1, "source": "../tilesets/terrain.tsj" } ],
      "layers": [ { "type": "tilelayer", "name": "a", "width": 1, "height": 1, "data": [1] } ] })");
    put(src, "maps/short.tmj", R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 16,
      "tileheight": 16, "layers": [ { "type": "tilelayer", "name": "a", "width": 4, "height": 4, "data": [1] } ] })");
    put(src, "levels/bad.world", R"({ "format": "Bowl.world", )");
    cook::Report report;
    CHECK_FALSE(cook::cook_folder(src, out, report));
    REQUIRE(report.errors.size() == 2);
    CHECK(report.errors[0].starts_with("maps/short.tmj: "));
    CHECK(report.errors[1].starts_with("levels/bad.world: "));
    CHECK(fs::exists(out / "tilesets/terrain.tsj")); // nothing is left out of a cook that failed
}
