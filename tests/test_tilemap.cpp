#include "bowl/gfx/tilemap.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <format>
#include <map>

using namespace bowl;
using namespace bowl::gfx;

// Implemented by stb_image_write inside bowl_gfx; reused here to build zlib-compressed layer data.
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
    load(R"({"orientation": "octagonal", "tilewidth": 8, "tileheight": 8})", &error);
    CHECK(error.find("octagonal") != std::string::npos);
    load(R"({"orientation": "orthogonal", "tilewidth": 8, "tileheight": 8,
             "tilesets": [ { "firstgid": 1, "source": "x.tsx" } ]})", &error);
    CHECK(error.find("maps/x.tsx") != std::string::npos);

    Tilemap map;
    auto none = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
    CHECK_FALSE(Tilemap::parse_tmx("<map><layer></map>", "", none, map, error));
    CHECK(error.find("XML") != std::string::npos);
}

namespace {

// The same terrain tileset as terrain_tsj, as Tiled writes .tsx files.
constexpr const char* terrain_tsx = R"(<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" tiledversion="1.11.0" name="terrain" tilewidth="16" tileheight="16" spacing="2" margin="1" tilecount="8" columns="4">
 <image source="../art/terrain.png" width="72" height="38"/>
 <tile id="1">
  <properties>
   <property name="solid" type="bool" value="true"/>
  </properties>
 </tile>
 <tile id="5">
  <animation>
   <frame tileid="5" duration="100"/>
   <frame tileid="6" duration="150"/>
  </animation>
 </tile>
</tileset>
)";

Tilemap load_tmx(const std::string& text) {
    std::map<std::string, std::string> files{{"tilesets/terrain.tsx", terrain_tsx}};
    auto read = [&](const std::string& path) -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    Tilemap map;
    std::string error;
    REQUIRE_MESSAGE(Tilemap::parse_tmx(text, "maps/", read, map, error), error);
    return map;
}

} // namespace

TEST_CASE("tilemap: .tmx maps read like .tmj ones") {
    std::string tmx = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" orientation="orthogonal" renderorder="right-down" width="4" height="3" tilewidth="16" tileheight="16" infinite="0" backgroundcolor="#80112233">
 <properties>
  <property name="music" value="town.ogg"/>
  <property name="speed" type="float" value="1.5"/>
  <property name="note">two
lines</property>
  <property name="weather" type="class" propertytype="Weather">
   <properties>
    <property name="rain" type="bool" value="true"/>
    <property name="wind" type="float" value="2.5"/>
   </properties>
  </property>
 </properties>
 <tileset firstgid="1" source="../tilesets/terrain.tsx"/>
 <tileset firstgid="9" name="props" tilewidth="32" tileheight="48" tilecount="1" columns="0">
  <tile id="0"><image source="tree.png" width="32" height="48"/></tile>
 </tileset>
 <layer id="1" name="ground" width="4" height="3">
  <data encoding="csv">
1,2,2,1,
1,6,0,1,
2147483649,1073741825,536870913,3221225473
</data>
 </layer>
 <group id="2" name="deco" offsetx="4" opacity="0.5">
  <layer id="3" name="packed" width="4" height="3" offsetx="1">
   <data encoding="base64" compression="zlib">
   {}
   </data>
  </layer>
  <objectgroup id="4" name="spawns">
   <object id="1" name="player" type="spawn" x="24" y="40">
    <properties><property name="facing" value="left"/></properties>
    <point/>
   </object>
   <object id="2" name="tree" gid="9" x="0" y="48" width="32" height="48"/>
   <object id="3" name="pond &amp; reeds" x="0" y="0"><polygon points="0,0 10,0 5,8"/></object>
  </objectgroup>
 </group>
 <layer id="5" name="xml" width="2" height="1">
  <data><tile gid="3"/><tile/></data>
 </layer>
 <imagelayer id="6" name="sky" visible="0"><image source="../art/sky.png" width="64" height="32"/></imagelayer>
</map>
)",
                                  zlib_base64({0, 0, 3, 0, 0, 0, 0, 0, 4, 0, 0, 0}));
    Tilemap map = load_tmx(tmx);
    CHECK(map.width == 4);
    CHECK(map.properties["music"].as_string() == "town.ogg");
    CHECK(map.properties["speed"].as_float() == doctest::Approx(1.5f));
    CHECK(map.properties["note"].as_string() == "two\nlines");
    CHECK(map.properties["weather"]["rain"].as_bool());
    CHECK(map.properties["weather"]["wind"].as_float() == doctest::Approx(2.5f));
    CHECK(map.background.a == doctest::Approx(0x80 / 255.0f));

    REQUIRE(map.tilesets.size() == 2);
    const Tileset& terrain = map.tilesets[0];
    CHECK(terrain.image_path == "art/terrain.png");
    CHECK(terrain.source_rect(5) == Rect{19, 19, 16, 16});
    REQUIRE(terrain.tile(5));
    CHECK(terrain.tile(5)->animation[1].seconds == doctest::Approx(0.15f));
    CHECK(map.tilesets[1].tiles[0].image_path == "maps/tree.png");

    const MapLayer* ground = map.layer("ground");
    REQUIRE(ground);
    CHECK(ground->at(1, 0) == 2);
    CHECK(map.tile_property("ground", 1, 0, "solid").as_bool());
    CHECK((ground->at(0, 2) & tile_flip_h) != 0);
    CHECK((ground->at(2, 2) & tile_flip_d) != 0);

    const MapLayer* packed = map.layer("deco/packed");
    REQUIRE(packed);
    CHECK(packed->at(2, 0) == 3);
    CHECK(packed->at(0, 2) == 4);
    CHECK(packed->offset == Vec2{5, 0});
    CHECK(packed->opacity == doctest::Approx(0.5f));

    const MapLayer* spawns = map.layer("deco/spawns");
    REQUIRE(spawns);
    REQUIRE(spawns->objects.size() == 3);
    CHECK(spawns->objects[0].shape == MapObject::Shape::Point);
    CHECK(spawns->objects[0].properties["facing"].as_string() == "left");
    CHECK(spawns->objects[1].shape == MapObject::Shape::Tile);
    CHECK(spawns->objects[1].gid == 9);
    CHECK(spawns->objects[2].name == "pond & reeds");
    REQUIRE(spawns->objects[2].points.size() == 3);
    CHECK(spawns->objects[2].points[2] == Vec2{5, 8});

    REQUIRE(map.layer("xml"));
    CHECK(map.layer("xml")->at(0, 0) == 3);
    CHECK(map.layer("xml")->at(1, 0) == 0);
    REQUIRE(map.layer("sky"));
    CHECK_FALSE(map.layer("sky")->visible);
}

TEST_CASE("tilemap: staggered and hexagonal maps find the cell under a point") {
    struct Case {
        const char* orientation;
        const char* axis;
        const char* index;
        int side;
    };
    for (Case c : {Case{"staggered", "y", "odd", 0}, Case{"staggered", "x", "even", 0}, Case{"hexagonal", "y", "odd", 12},
                   Case{"hexagonal", "x", "even", 14}, Case{"hexagonal", "y", "even", 16}}) {
        CAPTURE(c.orientation);
        CAPTURE(c.axis);
        CAPTURE(c.index);
        Tilemap map = load(std::format(R"({{"orientation": "{}", "staggeraxis": "{}", "staggerindex": "{}",
            "hexsidelength": {}, "width": 8, "height": 8, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []}})",
                                       c.orientation, c.axis, c.index, c.side));
        for (i32 y = 0; y < 8; ++y)
            for (i32 x = 0; x < 8; ++x) {
                // The middle of each cell's box is inside its hexagon or diamond.
                Vec2 center = map.cell_to_world(static_cast<f32>(x), static_cast<f32>(y)) + Vec2{16, 14};
                CHECK(map.cell_at(center) == std::pair{x, y});
            }
        CHECK(map.pixel_size().x > 8 * 16);
    }

    Tilemap hex = load(R"({"orientation": "hexagonal", "staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 12,
        "width": 4, "height": 4, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []})");
    CHECK(hex.cell_to_world(0, 0) == Vec2{0, 0});
    CHECK(hex.cell_to_world(0, 1) == Vec2{16, 20}); // odd rows shift right by half a tile, down by side + offset
    CHECK(hex.cell_to_world(1, 2) == Vec2{32, 40});
    CHECK(hex.pixel_size() == Vec2{4 * 32 + 16, 4 * 20 + 8});
}

TEST_CASE("paths normalize") {
    CHECK(normalize_path("maps/../art/a.png") == "art/a.png");
    CHECK(normalize_path("a/./b//c.png") == "a/b/c.png");
    CHECK(normalize_path("../x.png") == "../x.png");
}

TEST_CASE("tilemap: object templates fill in objects, the instance's own fields winning") {
    std::map<std::string, std::string> files{
        {"tilesets/terrain.tsj", terrain_tsj},
        {"templates/chest.tj", R"({ "type": "template", "tileset": { "firstgid": 1, "source": "../tilesets/terrain.tsj" },
            "object": { "gid": 3, "width": 16, "height": 16, "name": "chest", "type": "loot",
                        "properties": [ { "name": "gold", "type": "int", "value": 5 },
                                        { "name": "locked", "type": "bool", "value": true } ] } })"},
        {"templates/zone.tx", R"(<?xml version="1.0" encoding="UTF-8"?>
<template>
 <object name="zone" type="spawn" width="32" height="16">
  <properties><property name="kind" value="slime"/></properties>
 </object>
</template>)"}};
    auto read = [&](const std::string& path) -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    auto doc = json::parse(R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 16, "tileheight": 16,
      "tilesets": [ { "firstgid": 1, "name": "props", "tilewidth": 16, "tileheight": 16, "columns": 2, "tilecount": 8,
                      "image": "props.png" },
                    { "firstgid": 9, "source": "../tilesets/terrain.tsj" } ],
      "layers": [ { "type": "objectgroup", "name": "things", "objects": [
        { "id": 1, "template": "../templates/chest.tj", "x": 10, "y": 20,
          "properties": [ { "name": "gold", "type": "int", "value": 50 } ] },
        { "id": 2, "template": "../templates/zone.tx", "x": 64, "y": 0, "name": "north zone" } ] } ] })");
    REQUIRE(doc);
    Tilemap map;
    std::string error;
    REQUIRE_MESSAGE(Tilemap::parse(doc.value, "maps/", read, map, error), error);
    const MapLayer* things = map.layer("things");
    REQUIRE(things);
    REQUIRE(things->objects.size() == 2);
    const MapObject& chest = things->objects[0];
    CHECK(chest.name == "chest");
    CHECK(chest.type == "loot");
    CHECK(chest.bounds == Rect{10, 20, 16, 16});
    CHECK(chest.shape == MapObject::Shape::Tile);
    CHECK(chest.gid == 9 + 2); // the template's tile 3 in its own numbering is local tile 2 of terrain, at 9 here
    CHECK(chest.properties["gold"].as_int() == 50);
    CHECK(chest.properties["locked"].as_bool());
    const MapObject& zone = things->objects[1];
    CHECK(zone.name == "north zone");
    CHECK(zone.type == "spawn");
    CHECK(zone.bounds == Rect{64, 0, 32, 16});
    CHECK(zone.properties["kind"].as_string() == "slime");
}

TEST_CASE("tilemap: Wang sets as data, and terrain drawn from corners") {
    Tilemap map = load(R"({ "orientation": "orthogonal", "width": 2, "height": 1, "tilewidth": 16, "tileheight": 16,
      "tilesets": [ { "firstgid": 1, "name": "ground", "tilewidth": 16, "tileheight": 16, "columns": 4, "tilecount": 4,
        "image": "ground.png",
        "wangsets": [ { "name": "shore", "type": "corner", "tile": -1,
          "colors": [ { "name": "grass", "color": "#40a040", "tile": 0, "probability": 1 },
                      { "name": "water", "color": "#3060c0", "tile": 1, "probability": 1 } ],
          "wangtiles": [ { "tileid": 0, "wangid": [0, 1, 0, 1, 0, 1, 0, 1] },
                         { "tileid": 1, "wangid": [0, 2, 0, 2, 0, 2, 0, 2] },
                         { "tileid": 2, "wangid": [0, 1, 0, 1, 0, 1, 0, 2] },
                         { "tileid": 3, "wangid": [0, 1, 0, 1, 0, 1, 0, 1] } ] } ] } ],
      "layers": [] })");
    REQUIRE(map.tilesets.size() == 1);
    const WangSet* shore = map.tilesets[0].wang_set("shore");
    REQUIRE(shore);
    CHECK(shore->type == WangSet::Type::Corner);
    u8 grass = shore->terrain("grass"), water = shore->terrain("water");
    CHECK(grass == 1);
    CHECK(water == 2);
    CHECK(shore->terrains[1].color == Color::hex(0x3060c0));
    CHECK(shore->corner_tile(water, grass, grass, grass) == 2u);
    CHECK(shore->corner_tile(water, water, water, water) == 1u);
    CHECK_FALSE(shore->corner_tile(grass, water, grass, water)); // no tile drawn for that
    CHECK(shore->matching({0, 1, 0, 1, 0, 1, 0, 1}).size() == 2); // two plain-grass variants

    // Corners of a 2 x 1 strip: water at the top-left corner only.
    const u8 corners[] = {water, grass, grass, grass, grass, grass};
    MapLayer layer;
    CHECK(fill_from_corners(layer, map.tilesets[0], *shore, corners, 2, 1) == 0);
    CHECK(layer.at(0, 0) == 1 + 2u); // the shore corner tile
    u32 plain = layer.at(1, 0);
    CHECK((plain == 1 + 0u || plain == 1 + 3u));
    MapLayer again;
    fill_from_corners(again, map.tilesets[0], *shore, corners, 2, 1);
    CHECK(again.at(1, 0) == plain); // same corners, same choice
}

// Found by fuzzing (tests/fuzz).
TEST_CASE("tilemap: layers bigger than their data or than memory fail to load, without trying") {
    // Gids past 2^32 were undefined behavior to read.
    Tilemap map = load(R"({ "orientation": "orthogonal", "width": 2, "height": 1, "tilewidth": 8, "tileheight": 8,
        "layers": [ { "type": "tilelayer", "name": "g", "width": 2, "height": 1, "data": [1073741824000, -5] } ] })");
    REQUIRE(map.layer("g"));
    CHECK(map.layer("g")->cells.size() == 2);

    // A layer claiming a million by a million cells was allocated before its data was looked at.
    std::string error;
    load(R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 8, "tileheight": 8,
        "layers": [ { "type": "tilelayer", "name": "g", "width": 1000000, "height": 1000000, "data": [] } ] })", &error);
    CHECK(error.find("more than") != std::string::npos);
    load(R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 8, "tileheight": 8,
        "layers": [ { "type": "tilelayer", "name": "g", "width": 4, "height": 4, "data": [1, 2] } ] })", &error);
    CHECK(error.find("shorter") != std::string::npos);
    // Compressed data inflates to exactly the layer, not to whatever it asks.
    load(std::format(R"({{ "orientation": "orthogonal", "width": 2, "height": 1, "tilewidth": 8, "tileheight": 8,
        "layers": [ {{ "type": "tilelayer", "name": "g", "width": 2, "height": 1, "encoding": "base64",
                       "compression": "zlib", "data": "{}" }} ] }})",
                     zlib_base64({1, 2, 3, 4, 5, 6, 7, 8})),
         &error);
    CHECK(error.find("zlib") != std::string::npos);

    // Chunks whose corners overflowed 32 bits were written outside the layer, and chunks far apart made a layer of
    // billions of cells.
    load(R"({ "orientation": "orthogonal", "width": 0, "height": 0, "tilewidth": 8, "tileheight": 8, "infinite": true,
        "layers": [ { "type": "tilelayer", "name": "g", "chunks": [
            { "x": 2147483647, "y": 0, "width": 2, "height": 1, "data": [1, 2] },
            { "x": 0, "y": 0, "width": 1, "height": 1, "data": [3] } ] } ] })", &error);
    CHECK(error.find("out of range") != std::string::npos);
    load(R"({ "orientation": "orthogonal", "width": 0, "height": 0, "tilewidth": 8, "tileheight": 8, "infinite": true,
        "layers": [ { "type": "tilelayer", "name": "g", "chunks": [
            { "x": 0, "y": 0, "width": 1, "height": 1, "data": [1] },
            { "x": 100000000, "y": 100000000, "width": 1, "height": 1, "data": [2] } ] } ] })", &error);
    CHECK(error.find("more than a layer") != std::string::npos);
}

// Found by fuzzing: a point far outside a map (from a map whose sizes reach past 2^31 pixels) was converted to a cell
// number it didn't fit, which is undefined.
TEST_CASE("tilemap: points far off the map give far-off cells") {
    Tilemap map = load(R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 16, "tileheight": 16,
        "tilesets": [], "layers": [] })");
    CHECK(map.cell_at({1e30f, -1e30f}) == std::pair<i32, i32>{1 << 28, -(1 << 28)});
    CHECK(map.cell_at({std::nanf(""), 20}) == std::pair<i32, i32>{0, 1});
    Tilemap hex = load(R"({"orientation": "hexagonal", "staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 12,
        "width": 4, "height": 4, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []})");
    hex.cell_at({3e38f, 3e38f});
    hex.cell_at({-3e38f, -3e38f});
    hex.cell_to_world(3e38f, -3e38f);
    Tilemap diamonds = load(R"({"orientation": "staggered", "staggeraxis": "x", "staggerindex": "even",
        "width": 4, "height": 4, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []})");
    diamonds.cell_at({3e38f, -3e38f});
}
