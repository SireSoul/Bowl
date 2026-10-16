// Tiled maps: .tmj (JSON) and .tmx (XML), with the external tilesets (.tsj, .tsx) and object templates they refer to.
//
// Input: the map, then optionally a NUL byte and the file every path the map refers to reads as (a tileset or a
// template, which may refer onwards to itself).

#include "fuzz.hpp"

#include "bowl/gfx/tilemap.hpp"

using namespace bowl;
using namespace bowl::gfx;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<std::string_view> files = fuzz::split_files({data, size});
    std::string_view text = files[0];
    auto read = [&](const std::string&) -> std::optional<std::string> {
        if (files.size() < 2) return std::nullopt;
        return std::string(files[1]);
    };
    Tilemap map;
    std::string error;
    bool ok = false;
    if (text.find_first_not_of(" \t\r\n") != std::string_view::npos && text[text.find_first_not_of(" \t\r\n")] == '<') {
        ok = Tilemap::parse_tmx(text, "maps/", read, map, error);
    } else {
        auto doc = json::parse(text);
        if (!doc) return 0;
        ok = Tilemap::parse(doc.value, "maps/", read, map, error);
    }
    if (!ok) return 0;

    // What a game does with a map it loaded: look at cells and their tiles, convert coordinates, draw it.
    for (const MapLayer& layer : map.layers) {
        map.layer(layer.name);
        u32 cells = 0;
        for (i32 y = layer.origin_y; y < layer.origin_y + static_cast<i32>(std::min(layer.height, 64u)); ++y)
            for (i32 x = layer.origin_x; x < layer.origin_x + static_cast<i32>(std::min(layer.width, 64u)); ++x) {
                u32 gid = layer.at(x, y);
                map.tileset_for(gid);
                map.tile_info(gid);
                map.tile_property(layer.name, x, y, "solid");
                if (++cells > 4096) break;
            }
    }
    for (const Tileset& tileset : map.tilesets) {
        for (u32 id = 0; id < std::min(tileset.tile_count, 64u); ++id) {
            tileset.tile(id);
            tileset.source_rect(id);
        }
        for (const WangSet& set : tileset.wang_sets) {
            set.matching({});
            u8 terrains = static_cast<u8>(std::min<usize>(set.terrains.size(), 255));
            set.corner_tile(terrains, 1, 0, terrains, 7);
            const u8 corners[] = {1, terrains, 0, 1, terrains, 1};
            MapLayer filled;
            fill_from_corners(filled, tileset, set, corners, 2, 1);
        }
    }
    Vec2 corner = map.cell_to_world(static_cast<f32>(map.width), static_cast<f32>(map.height));
    map.world_to_cell(corner);
    map.cell_at(corner * 0.5f);
    map.pixel_size();
    SpriteBatch batch;
    batch.begin(Camera2D::screen({320, 180}));
    draw_tilemap(batch, map, {.time = 1.5f, .y_sort = true});
    return 0;
}
