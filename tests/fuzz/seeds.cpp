// Writes the fuzzers' first inputs (their seed corpora), one folder per fuzzer, from the documents, fonts, pictures and
// packets the tests use. A fuzzer that starts from valid inputs reaches the deep parts of a format in minutes instead
// of hours.
//
//     bowl_fuzz_seeds <folder>

#include "fuzz.hpp"

#include "../tiny_font.hpp"

#include "bowl/core/archive.hpp"
#include "bowl/core/file.hpp"
#include "bowl/core/json.hpp"
#include "bowl/gfx/bwtx.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/net/socket.hpp"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h> // implemented inside bowl_gfx
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>

using namespace bowl;
using namespace bowl::testing;
namespace fs = std::filesystem;

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace {

fs::path root;

void seed(std::string_view fuzzer, std::span<const u8> bytes) {
    static std::unordered_map<std::string, int> counts;
    fs::path dir = root / fuzzer;
    fs::create_directories(dir);
    int n = counts[std::string(fuzzer)]++;
    if (!write_file_bytes(dir / std::format("seed_{:02}", n), bytes.data(), bytes.size())) {
        std::fprintf(stderr, "bowl_fuzz_seeds: can't write into %s\n", dir.string().c_str());
        std::exit(1);
    }
}
void seed(std::string_view fuzzer, std::string_view text) {
    seed(fuzzer, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}
// Inputs of several files: split at NUL bytes (see fuzz::split_files).
void seed(std::string_view fuzzer, std::initializer_list<std::string_view> files) {
    std::string all;
    for (std::string_view f : files) {
        if (!all.empty()) all += '\0';
        all += f;
    }
    seed(fuzzer, std::string_view(all));
}

// JSON (tests/test_json.cpp, test_reflect.cpp, test_localization.cpp)

void json_seeds() {
    const char* docs[] = {
        R"({
        "name": "hero", "frames": [1, 2.5, -3e2], "loop": true, "parent": null,
        "nested": {"size": {"w": 32, "h": 48}}
    })",
        R"(["a\"b\\c\n", "é€", "😀", "raw é", "é😀"])",
        "// config\n{ /* speed */ \"speed\": 4, \"tags\": [\"a\", \"b\",], }",
        R"({ "name": "Ada", "n": [0, 1, -1, 300, -70000, 1.5, 0.1, 1e300, 9007199254740993, -0.0],
        "flags": [true, false, null], "nested": { "name": "Ada", "deep": [[[]], {}] } })",
        R"({
  // Enemies of the first area.
  "slime": {
    "hp": 30, // tune later
    "speed": 1.5,
    "drops": ["gel"],
  },
  /* bats come at night */
  "bat": { "hp": 8, "tags": [] }
}
)",
        R"({ "menu": { "start": "Start", "quit": "Quit" }, "greeting": "Hello, {name}!",
        "coins": { "one": "{n} coin", "other": "{n} coins" } })",
    };
    for (const char* doc : docs) {
        seed("json", doc);
        seed("json", json::encode_binary(json::parse(doc).value));
    }
}

// XML and Tiled maps (tests/test_xml.cpp, test_tilemap.cpp)

constexpr const char* xml_doc = R"(<?xml version="1.0"?>
<!-- a comment -->
<!DOCTYPE map>
<map name="a &amp; b" quote='it&apos;s'>
  <layer id="1"/>
  <note>x &lt; y &#65;&#x42; <![CDATA[<raw> & stuff]]></note>
  <layer id="2"></layer>
</map>
)";

constexpr const char* terrain_tsj = R"({
  "name": "terrain", "tilewidth": 16, "tileheight": 16, "columns": 4, "tilecount": 8, "margin": 1, "spacing": 2,
  "image": "../art/terrain.png", "imagewidth": 72, "imageheight": 38, "type": "tileset", "version": "1.10",
  "tiles": [
    { "id": 1, "properties": [ { "name": "solid", "type": "bool", "value": true } ] },
    { "id": 5, "animation": [ { "tileid": 5, "duration": 100 }, { "tileid": 6, "duration": 100 } ] }
  ]
})";

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

void tilemap_seeds() {
    seed("xml", xml_doc);
    seed("xml", terrain_tsx);
    std::string packed = zlib_base64({0, 0, 3, 0, 0, 0, 0, 0, 4, 0, 0, 0});
    std::string ortho = std::format(R"({{
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
                                    packed);
    seed("tilemap", {ortho, terrain_tsj});
    seed("tilemap", {R"({
      "orientation": "orthogonal", "width": 0, "height": 0, "tilewidth": 8, "tileheight": 8, "infinite": true,
      "tilesets": [ { "firstgid": 1, "source": "../tilesets/terrain.tsj" } ],
      "layers": [ { "type": "tilelayer", "name": "ground", "startx": -2, "starty": 0, "width": 4, "height": 2,
        "chunks": [ { "x": -2, "y": 0, "width": 2, "height": 2, "data": [1, 0, 0, 2] },
                    { "x": 0, "y": 0, "width": 2, "height": 2, "data": [3, 0, 0, 4] } ] } ]
    })",
                     terrain_tsj});
    seed("tilemap", R"({ "orientation": "isometric", "width": 10, "height": 10, "tilewidth": 64, "tileheight": 32,
      "tilesets": [], "layers": [] })");
    seed("tilemap", R"({"orientation": "hexagonal", "staggeraxis": "y", "staggerindex": "odd", "hexsidelength": 12,
        "width": 4, "height": 4, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []})");
    seed("tilemap", R"({"orientation": "staggered", "staggeraxis": "x", "staggerindex": "even",
        "width": 8, "height": 8, "tilewidth": 32, "tileheight": 28, "tilesets": [], "layers": []})");
    seed("tilemap", {R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 16, "tileheight": 16,
      "tilesets": [ { "firstgid": 1, "name": "props", "tilewidth": 16, "tileheight": 16, "columns": 2, "tilecount": 8,
                      "image": "props.png" } ],
      "layers": [ { "type": "objectgroup", "name": "things", "objects": [
        { "id": 1, "template": "../templates/chest.tj", "x": 10, "y": 20,
          "properties": [ { "name": "gold", "type": "int", "value": 50 } ] } ] } ] })",
                     R"({ "type": "template", "tileset": { "firstgid": 1, "source": "../tilesets/terrain.tsj" },
            "object": { "gid": 3, "width": 16, "height": 16, "name": "chest", "type": "loot",
                        "properties": [ { "name": "gold", "type": "int", "value": 5 },
                                        { "name": "locked", "type": "bool", "value": true } ] } })"});
    seed("tilemap", {R"({ "orientation": "orthogonal", "width": 4, "height": 4, "tilewidth": 16, "tileheight": 16,
      "tilesets": [],
      "layers": [ { "type": "objectgroup", "name": "things", "objects": [
        { "id": 2, "template": "../templates/zone.tx", "x": 64, "y": 0, "name": "north zone" } ] } ] })",
                     R"(<?xml version="1.0" encoding="UTF-8"?>
<template>
 <object name="zone" type="spawn" width="32" height="16">
  <properties><property name="kind" value="slime"/></properties>
 </object>
</template>)"});
    seed("tilemap", R"({ "orientation": "orthogonal", "width": 2, "height": 1, "tilewidth": 16, "tileheight": 16,
      "tilesets": [ { "firstgid": 1, "name": "ground", "tilewidth": 16, "tileheight": 16, "columns": 4, "tilecount": 4,
        "image": "ground.png",
        "wangsets": [ { "name": "shore", "type": "corner", "tile": -1,
          "colors": [ { "name": "grass", "color": "#40a040", "tile": 0, "probability": 1 },
                      { "name": "water", "color": "#3060c0", "tile": 1, "probability": 1 } ],
          "wangtiles": [ { "tileid": 0, "wangid": [0, 1, 0, 1, 0, 1, 0, 1] },
                         { "tileid": 1, "wangid": [0, 2, 0, 2, 0, 2, 0, 2] },
                         { "tileid": 2, "wangid": [0, 1, 0, 1, 0, 1, 0, 2] },
                         { "tileid": 3, "wangid": [0, 1, 0, 1, 0, 1, 0, 1] } ] } ] } ],
      "layers": [ { "type": "tilelayer", "name": "ground", "width": 2, "height": 1, "data": [1, 3] } ] })");
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
                                  packed);
    seed("tilemap", {tmx, terrain_tsx});
    seed("xml", tmx);
}

// Aseprite sprite sheets (tests/test_sprite_sheet.cpp)

void sprite_sheet_seeds() {
    seed("sprite_sheet", R"({
  "frames": [
    { "filename": "hero 0.aseprite", "frame": { "x": 0, "y": 0, "w": 16, "h": 24 }, "rotated": false, "trimmed": true,
      "spriteSourceSize": { "x": 8, "y": 8, "w": 16, "h": 24 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 100 },
    { "filename": "hero 1.aseprite", "frame": { "x": 16, "y": 0, "w": 32, "h": 32 }, "rotated": false, "trimmed": false,
      "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 100 },
    { "filename": "hero 2.aseprite", "frame": { "x": 48, "y": 0, "w": 32, "h": 32 }, "rotated": false, "trimmed": false,
      "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 }, "sourceSize": { "w": 32, "h": 32 }, "duration": 200 }
  ],
  "meta": {
    "app": "https://www.aseprite.org/", "image": "hero.png", "format": "RGBA8888", "size": { "w": 112, "h": 32 },
    "frameTags": [
      { "name": "idle", "from": 0, "to": 1, "direction": "forward", "color": "#000000ff" },
      { "name": "swing", "from": 1, "to": 2, "direction": "reverse", "repeat": "1", "color": "#000000ff" },
      { "name": "bob", "from": 0, "to": 2, "direction": "pingpong", "color": "#000000ff" },
      { "name": "back", "from": 0, "to": 2, "direction": "pingpong_reverse", "repeat": "2" }
    ]
  }
})");
    seed("sprite_sheet", R"({
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
})");
}

// Saved worlds and prefabs (tests/test_reflect.cpp), as text and binary

void world_seeds() {
    const char* docs[] = {
        R"({ "format": "Bowl.world", "version": 1, "entities": [
        { "id": 0, "components": { "game::OldHero": { "title": "Ada", "lvl": 4 }, "game::Removed": {} } } ] })",
        R"({ "format": "Bowl.world", "version": 1, "game_version": 2, "entities": [
        { "id": 0, "components": { "game::Hero": { "name": "Ada", "level": 3, "alive": true, "faction": 1,
            "position": [1, 2], "facing": [0, 1, 0], "tint": [1, 0.5, 0.25, 1], "bounds": [0, 0, 16, 32],
            "stats": { "strength": 7, "speed": 1.5, "luck": 200, "gold": 999 }, "inventory": [1, 2, 3],
            "slots": [4, 5, 6], "buff": 0.5, "companion": 1, "party": [0, 1, null], "seed": 12345, "played": 3600.5,
            "extra": { "any": ["json"] } }, "game::Asleep": {} } },
        { "id": 1, "components": { "game::Hero": { "name": "Bo", "companion": 0 }, "game::Stats": { "luck": 3 } } } ] })",
        R"({ "components": { "game::Hero": { "name": "Guard", "level": 5 }, "game::Asleep": {},
        "game::NotARealComponent": { "x": 1 } } })",
    };
    for (const char* doc : docs) {
        seed("world", doc);
        seed("world", json::encode_binary(json::parse(doc).value));
    }
}

// Asset archives (tests/test_assets.cpp)

void archive_seeds() {
    fs::path src = fs::temp_directory_path() / "bowl_fuzz_seeds_archive";
    fs::remove_all(src);
    fs::create_directories(src / "sub");
    write_file_bytes(src / "a.txt", "alpha", 5);
    write_file_bytes(src / "sub" / "b.txt", "beta", 4);
    write_archive(src / "pack.bowlpak", src);
    seed("archive", *read_file_bytes(src / "pack.bowlpak"));
    seed("archive", "BWPK but not really");
    fs::remove_all(src);
}

// Fonts (tests/test_text.cpp) and pictures

void font_seeds() {
    seed("font", *read_file_bytes(fs::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "fonts" / "Roboto-Medium.ttf"));
    seed("font", TinyFont().build());
    TinyFont colr;
    add_colr(colr);
    seed("font", colr.build());
    TinyFont cbdt;
    add_cbdt(cbdt, png_of(20, 0, 200, 0));
    seed("font", cbdt.build());
    TinyFont sbix;
    add_sbix(sbix, png_of(20, 250, 120, 0));
    seed("font", sbix.build());
    TinyFont pictures_only; // like Noto Color Emoji: no outlines
    std::erase_if(pictures_only.tables, [](const auto& t) { return t.first == "glyf" || t.first == "loca"; });
    add_cbdt(pictures_only, png_of(4, 0, 200, 0));
    seed("font", pictures_only.build());
}

void png_seeds() {
    seed("png", png_of(1, 255, 0, 0));
    seed("png", png_of(5, 10, 20, 30));
    seed("png", *read_file_bytes(fs::path(BOWL_SAMPLES_DIR) / "icon.png"));
    // Other formats the texture loader reads
    const u8 pixels[4 * 4 * 4] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 9, 9, 9, 9};
    auto into = [](void* context, void* data, int n) {
        auto* out = static_cast<std::vector<u8>*>(context);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + n);
    };
    std::vector<u8> bmp, tga, jpg;
    stbi_write_bmp_to_func(into, &bmp, 4, 4, 4, pixels);
    stbi_write_tga_to_func(into, &tga, 4, 4, 4, pixels);
    stbi_write_jpg_to_func(into, &jpg, 4, 4, 3, pixels, 80);
    seed("png", bmp);
    seed("png", tga);
    seed("png", jpg);
}

// Cooked pictures (tests/test_cook.cpp): every kind of step the format has, and a real picture. The texture loader
// reads them too, so they also seed the png fuzzer.
void bwtx_seeds() {
    auto both = [](const std::vector<u8>& bytes) {
        seed("bwtx", bytes);
        seed("png", bytes);
    };
    both(gfx::encode_bwtx(1, 1, std::vector<u8>{255, 0, 0, 255}));
    std::vector<u8> steps;
    for (int i = 0; i < 100; ++i) steps.insert(steps.end(), {10, 20, 30, 255}); // a run longer than 62
    for (int i = 0; i < 64; ++i) // small and medium changes, wrapping past 255
        steps.insert(steps.end(), {static_cast<u8>(250 + i), static_cast<u8>(i * 3), static_cast<u8>(i * 7), 255});
    for (int i = 0; i < 16; ++i) { // colours seen before, big jumps, alpha changing
        steps.insert(steps.end(), {255, 0, 0, 255});
        steps.insert(steps.end(), {static_cast<u8>(i * 97), static_cast<u8>(i * 31), 200, static_cast<u8>(i * 16)});
    }
    both(gfx::encode_bwtx(14, 14, steps));
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    if (gfx::decode_png(*read_file_bytes(fs::path(BOWL_SAMPLES_DIR) / "icon.png"), w, h, rgba))
        both(gfx::encode_bwtx(w, h, rgba));
}

// Network packets (tests/test_net.cpp); see fuzz_host.cpp and fuzz_rendezvous.cpp for the steps

void put16(std::vector<u8>& out, u16 v) { out.push_back(static_cast<u8>(v)), out.push_back(static_cast<u8>(v >> 8)); }
void put32(std::vector<u8>& out, u32 v) { put16(out, static_cast<u16>(v)), put16(out, static_cast<u16>(v >> 16)); }
void put64(std::vector<u8>& out, u64 v) { put32(out, static_cast<u32>(v)), put32(out, static_cast<u32>(v >> 32)); }

constexpr u32 protocol = 0x424F574C, rendezvous_magic = 0x5244564E;

// The encrypted part of a Data packet: sequence, acks, then messages.
std::vector<u8> data_body(u16 sequence, u16 ack, u32 ack_bits, bool acks,
                          std::initializer_list<std::string_view> unreliable,
                          std::initializer_list<std::pair<u16, std::string_view>> reliable, bool more = false) {
    std::vector<u8> out;
    put16(out, sequence), put16(out, ack), put32(out, ack_bits);
    out.push_back(acks ? 1 : 0);
    for (std::string_view m : unreliable) {
        out.push_back(0);
        put16(out, static_cast<u16>(m.size()));
        out.insert(out.end(), m.begin(), m.end());
    }
    for (auto [id, m] : reliable) {
        out.push_back(more ? 3 : 1);
        put16(out, id);
        put16(out, static_cast<u16>(m.size()));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

void net_seeds() {
    using fuzz::Output;
    // Steps (fuzz_host.cpp): what happens in the low bits, who a packet is from in bits 4 and 5.
    constexpr u8 to_server = 0, to_client = 1, sealed_to_server = 2, sealed_to_client = 3, introduce = 4, wait = 5,
                 send = 6, hang_up = 7;
    constexpr u8 from_rendezvous = 2 << 4, from_stranger = 3 << 4;
    std::vector<u8> addresses;
    net::Address::loopback(27015).write(addresses);
    net::Address::loopback(27015, true).write(addresses);
    seed("address", addresses);

    { // messages both ways, acknowledged
        Output o;
        o.byte(sealed_to_server), o.chunk(data_body(1, 0, 0, true, {"hi"}, {{0, "hello"}}));
        o.byte(send), o.chunk(std::vector<u8>(3000, 'x')); // three reliable pieces
        o.byte(wait), o.byte(5);
        o.byte(sealed_to_client), o.chunk(data_body(1, 1, 1, true, {}, {{0, "a"}, {2, "c"}, {1, "b"}}));
        o.byte(sealed_to_server), o.chunk(data_body(2, 2, 3, true, {}, {{1, "more"}, {2, "end"}}, true));
        o.byte(wait), o.byte(30);
        o.byte(sealed_to_server | 0x80), o.chunk({});
        seed("host", o.bytes);
    }
    { // a stranger asks to connect, three times over, then everyone goes quiet
        std::vector<u8> request;
        put32(request, protocol);
        request.push_back(1);
        put64(request, 0x1234);
        for (int i = 0; i < 32; ++i) request.push_back(static_cast<u8>(9 + i));
        request.resize(request.size() + 24);
        Output o;
        for (int i = 0; i < 3; ++i) o.byte(to_server | from_stranger), o.chunk(request);
        o.byte(send | 0x40 | 0x80), o.chunk(std::vector<u8>(100, 'u'));
        for (int i = 0; i < 4; ++i) o.byte(wait), o.byte(255);
        seed("host", o.bytes);
        // The same request through the relay
        std::vector<u8> relayed;
        put32(relayed, rendezvous_magic);
        relayed.push_back(5);
        put64(relayed, 77);
        relayed.insert(relayed.end(), request.begin(), request.end());
        Output r;
        r.byte(to_server | from_rendezvous), r.chunk(relayed);
        seed("host", r.bytes);
    }
    { // rooms: the rendezvous server introduces a player to the host, and the client to a room's host
        std::vector<u8> player;
        put32(player, rendezvous_magic);
        player.push_back(3);
        put64(player, 0);
        put64(player, 42);
        player.push_back(1);
        net::Address::loopback(4000).write(player);
        std::vector<u8> session;
        put64(session, 43);
        session.push_back(1);
        net::Address::loopback(4001).write(session);
        Output o;
        o.byte(to_server | from_rendezvous), o.chunk(player);
        o.byte(introduce), o.chunk(session);
        o.byte(wait), o.byte(120);
        o.byte(to_client | from_rendezvous), o.chunk(player);
        seed("host", o.bytes);
        Output none;
        none.byte(introduce | 0x80), none.chunk({});
        none.byte(hang_up | 0x80);
        seed("host", none.bytes);
    }

    // Rendezvous: a host announces a room, a player joins it (directly, then relayed), and relays a packet each way.
    auto text = [](std::vector<u8>& out, std::string_view s) {
        out.push_back(static_cast<u8>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    };
    std::vector<u8> host_room, join, join_relayed, relay;
    put32(host_room, rendezvous_magic), host_room.push_back(1), put32(host_room, protocol), text(host_room, "room");
    put32(join, rendezvous_magic), join.push_back(2), put32(join, protocol), text(join, "room"), put64(join, 99);
    join.push_back(0);
    join_relayed = join;
    join_relayed.back() = 1;
    join_relayed[join_relayed.size() - 2] = 98;
    put32(relay, rendezvous_magic), relay.push_back(5), put64(relay, 1);
    relay.insert(relay.end(), 40, 7);
    Output o;
    o.byte(0), o.chunk(host_room);
    o.byte(1), o.chunk(join);
    o.byte(1), o.chunk(join); // asked again
    o.byte(2), o.chunk(join_relayed);
    o.byte(1), o.chunk(relay);
    o.byte(0), o.chunk(relay);
    o.byte(3 | 40 << 2), o.chunk(host_room); // a while later, someone else takes the room
    seed("rendezvous", o.bytes);
    Output nobody;
    nobody.byte(1), nobody.chunk(join);
    seed("rendezvous", nobody.bytes);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: bowl_fuzz_seeds <folder>\n");
        return 1;
    }
    root = argv[1];
    json_seeds();
    tilemap_seeds();
    sprite_sheet_seeds();
    world_seeds();
    archive_seeds();
    font_seeds();
    png_seeds();
    bwtx_seeds();
    net_seeds();
    return 0;
}
