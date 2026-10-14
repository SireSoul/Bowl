#include "ph/gfx/tilemap.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <format>
#include <map>

using namespace ph;
using namespace ph::gfx;

// Implemented by stb_image_write inside ph_gfx; reused here to build zlib-compressed layer data.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace {

std::string base64(const std::vector<u8>& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    u32 buffer = 0;
    int bits = 0;
    for (u8 b : bytes) {
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

std::string zlib_base64(const std::vector<u32>& gids) {
    std::vector<u8> raw;
    for (u32 g : gids)
        for (int i = 0; i < 4; ++i) raw.push_back(static_cast<u8>(g >> (8 * i)));
    int len = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &len, 8);
    std::string out = base64(std::vector<u8>(z, z + len));
    std::free(z);
    return out;
}

// External tileset, as Tiled writes .tsj files.
constexpr const char* terrain_tsj = R"({
  "name": "terrain", "tilewidth": 16, "tileheight": 16, "columns": 4, "tilecount": 8, "margin": 1, "spacing": 2,
  "image": "../art/terrain.png", "imagewidth": 72, "imageheight": 38, "type": "tileset", "version": "1.10",
  "tiles": [
    { "id": 1, "properties": [ { "name": "solid", "type": "bool", "value": true } ] },
    { "id": 5, "animation": [ { "tileid": 5, "duration": 100 }, { "tileid": 6, "duration": 100 } ] }
  ]
})";

Tilemap load(const std::string& map_json, std::string* error_out = nullptr) {
    std::map<std::string, std::string> files{{"tilesets/terrain.tsj", terrain_tsj}};
    auto read = [&](const std::string& path) -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    auto doc = json::parse(map_json);
    REQUIRE(doc);
    Tilemap map;
    std::string error;
    bool ok = Tilemap::parse(doc.value, "maps/", read, map, error);
    if (error_out) *error_out = error;
    else REQUIRE_MESSAGE(ok, error);
    return map;
}

std::string ortho_map() {
    return std::format(R"({{
      "orientation": "orthogonal", "renderorder": "right-down", "width": 4, "height": 3,
      "tilewidth": 16, "tileheight": 16, "infinite": false, "backgroundcolor": "#80112233",
      "properties": [ {{ "name": "music", "type": "string", "value": "town.ogg" }} ],
      "tilesets": [ {{ "firstgid": 1, "source": "../tilesets/terrain.tsj" }},
                    {{ "firstgid": 9, "name": "props", "tilewidth": 32, "tileheight": 48, "columns": 0, "tilecount": 1,
                       "tiles": [ {{ "id": 0, "image": "tree.png", "imagewidth": 32, "imageheight": 48 }} ] }} ],
      "layers": [
        {{ "type": "tilelayer", "name": "ground", "width": 4, "height": 3, "x": 0, "y": 0, "opacity": 1, "visible": true,
           "data": [1, 2, 2, 1,  1, 6, 0, 1,  2147483649, 1073741825, 536870913, 3221225473] }},
        {{ "type": "group", "name": "deco", "offsetx": 4, "opacity": 0.5, "layers": [
          {{ "type": "tilelayer", "name": "packed", "width": 4, "height": 3, "encoding": "base64", "compression": "zlib",
             "offsetx": 1, "data": "{}" }},
          {{ "type": "objectgroup", "name": "spawns", "objects": [
            {{ "id": 1, "name": "player", "type": "spawn", "x": 24, "y": 40, "width": 0, "height": 0, "point": true,
               "properties": [ {{ "name": "facing", "type": "string", "value": "left" }} ] }},
            {{ "id": 2, "name": "tree", "gid": 9, "x": 0, "y": 48, "width": 32, "height": 48 }},
            {{ "id": 3, "name": "pond", "x": 0, "y": 0, "polygon": [ {{"x":0,"y":0}}, {{"x":10,"y":0}}, {{"x":5,"y":8}} ] }}
          ] }}
        ] }},
        {{ "type": "imagelayer", "name": "sky", "image": "../art/sky.png" }}
      ]
    }})",
                       zlib_base64({0, 0, 3, 0, 0, 0, 0, 0, 4, 0, 0, 0}));
}

} // namespace

TEST_CASE("tilemap: orthogonal map with external tileset, properties, flips, groups") {
    Tilemap map = load(ortho_map());
    CHECK(map.orientation == Tilemap::Orientation::Orthogonal);
    CHECK(map.width == 4);
    CHECK(map.properties["music"].as_string() == "town.ogg");
    CHECK(map.background.a == doctest::Approx(0x80 / 255.0f));

    REQUIRE(map.tilesets.size() == 2);
    const Tileset& terrain = map.tilesets[0];
    CHECK(terrain.image_path == "art/terrain.png"); // relative to the .tsj, normalized
    CHECK(terrain.source_rect(5) == Rect{1 + 1 * 18, 1 + 1 * 18, 16, 16}); // margin + spacing
    CHECK(map.tilesets[1].tiles[0].image_path == "maps/tree.png");

    const MapLayer* ground = map.layer("ground");
    REQUIRE(ground);
    CHECK(ground->at(1, 0) == 2);
    CHECK(ground->at(9, 9) == 0);
    CHECK(map.tile_property("ground", 1, 0, "solid").as_bool());
    CHECK_FALSE(map.tile_property("ground", 0, 0, "solid").as_bool());
    // Flip flags survive in the cell but not in lookups.
    u32 flipped = ground->at(0, 2);
    CHECK((flipped & tile_flip_h) != 0);
    CHECK(map.tileset_for(flipped) == &terrain);
    CHECK((ground->at(2, 2) & tile_flip_d) != 0);

    const MapLayer* packed = map.layer("deco/packed");
    REQUIRE(packed);
    CHECK(packed->at(2, 0) == 3);
    CHECK(packed->at(0, 2) == 4);
    CHECK(packed->offset == Vec2{5, 0}); // group + layer offsets add
    CHECK(packed->opacity == doctest::Approx(0.5f));

    const MapLayer* spawns = map.layer("deco/spawns");
    REQUIRE(spawns);
    REQUIRE(spawns->objects.size() == 3);
    CHECK(spawns->objects[0].shape == MapObject::Shape::Point);
    CHECK(spawns->objects[0].properties["facing"].as_string() == "left");
    CHECK(spawns->objects[1].shape == MapObject::Shape::Tile);
    CHECK(spawns->objects[2].points.size() == 3);

    REQUIRE(map.layer("sky"));
    CHECK(map.layer("sky")->image_path == "art/sky.png");
}

TEST_CASE("tilemap: infinite maps merge chunks") {
    Tilemap map = load(R"({
      "orientation": "orthogonal", "width": 0, "height": 0, "tilewidth": 8, "tileheight": 8, "infinite": true,
      "tilesets": [ { "firstgid": 1, "source": "../tilesets/terrain.tsj" } ],
      "layers": [ { "type": "tilelayer", "name": "ground", "startx": -2, "starty": 0, "width": 4, "height": 2,
        "chunks": [ { "x": -2, "y": 0, "width": 2, "height": 2, "data": [1, 0, 0, 2] },
                    { "x": 0, "y": 0, "width": 2, "height": 2, "data": [3, 0, 0, 4] } ] } ]
    })");
    const MapLayer* ground = map.layer("ground");
    REQUIRE(ground);
    CHECK(ground->origin_x == -2);
    CHECK(ground->width == 4);
    CHECK(ground->at(-2, 0) == 1);
    CHECK(ground->at(-1, 1) == 2);
    CHECK(ground->at(0, 0) == 3);
    CHECK(ground->at(1, 1) == 4);
}

TEST_CASE("tilemap: isometric coordinates round-trip") {
    Tilemap map = load(R"({
      "orientation": "isometric", "width": 10, "height": 10, "tilewidth": 64, "tileheight": 32,
      "tilesets": [], "layers": []
    })");
    // Tile (0,0)'s top corner sits at the horizontal middle of the map's bounding diamond.
    CHECK(map.cell_to_world(0, 0) == Vec2{320, 0});
    CHECK(map.cell_to_world(1, 0) == Vec2{352, 16});
    CHECK(map.cell_to_world(0, 1) == Vec2{288, 16});
    for (Vec2 c : {Vec2{0, 0}, Vec2{3.5f, 7.25f}, Vec2{9, 2}}) {
        Vec2 back = map.world_to_cell(map.cell_to_world(c.x, c.y));
        CHECK(back.x == doctest::Approx(c.x));
        CHECK(back.y == doctest::Approx(c.y));
    }
}

TEST_CASE("tilemap: helpful errors") {
    std::string error;
    load(R"({"orientation": "hexagonal", "tilewidth": 8, "tileheight": 8})", &error);
    CHECK(error.find("hexagonal") != std::string::npos);
    load(R"({"orientation": "orthogonal", "tilewidth": 8, "tileheight": 8,
             "tilesets": [ { "firstgid": 1, "source": "x.tsx" } ]})", &error);
    CHECK(error.find("JSON") != std::string::npos);
}

TEST_CASE("paths normalize") {
    CHECK(normalize_path("maps/../art/a.png") == "art/a.png");
    CHECK(normalize_path("a/./b//c.png") == "a/b/c.png");
    CHECK(normalize_path("../x.png") == "../x.png");
}
