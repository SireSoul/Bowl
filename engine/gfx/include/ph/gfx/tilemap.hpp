#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/json.hpp"
#include "ph/gfx/sprites.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ph::gfx {

// Tiled maps (mapeditor.org), loaded from Tiled's JSON format: maps as .tmj, external tilesets as .tsj.
// Orthogonal and isometric (diamond) orientations; finite and infinite maps; CSV, base64 and base64+zlib layer data.

inline constexpr u32 tile_flip_h = 0x80000000u;
inline constexpr u32 tile_flip_v = 0x40000000u;
inline constexpr u32 tile_flip_d = 0x20000000u; // anti-diagonal (transpose)
inline constexpr u32 tile_gid_mask = 0x0FFFFFFFu;

struct TileFrame {
    u32 tile; // local id within the tileset
    f32 seconds;
};

struct TileInfo {
    u32 id = 0; // local id
    json::Value properties;
    std::string type;
    std::vector<TileFrame> animation;
    // Image-collection tilesets: each tile has its own image.
    std::string image_path;
    Asset<Texture> image;
    Vec2 image_size;
};

struct Tileset {
    std::string name;
    u32 first_gid = 1;
    u32 tile_width = 0, tile_height = 0;
    u32 columns = 0, tile_count = 0, margin = 0, spacing = 0;
    Vec2 tile_offset;
    std::string image_path; // empty for image collections
    Asset<Texture> image;
    std::vector<TileInfo> tiles; // only tiles with extra data (properties, animation, own image)
    json::Value properties;

    const TileInfo* tile(u32 local_id) const;
    Rect source_rect(u32 local_id) const; // pixels in `image`
};

struct MapObject {
    enum class Shape : u8 { Rectangle, Point, Ellipse, Polygon, Polyline, Tile, Text };
    u32 id = 0;
    std::string name, type;
    Shape shape = Shape::Rectangle;
    Rect bounds;  // x,y = Tiled's object position (pixels); tile objects are anchored bottom-left as in Tiled
    f32 rotation = 0; // degrees, as in Tiled
    u32 gid = 0;      // tile objects
    std::vector<Vec2> points; // polygon/polyline, relative to (bounds.x, bounds.y)
    bool visible = true;
    json::Value properties;
};

struct MapLayer {
    enum class Kind : u8 { Tiles, Objects, Image };
    Kind kind = Kind::Tiles;
    std::string name; // group layers flatten to "group/child"
    bool visible = true;
    f32 opacity = 1;
    Color tint = Color::white();
    Vec2 offset;
    json::Value properties;
    // Tiles: a rectangle of cells (infinite maps are merged into one rectangle starting at origin_x/y).
    i32 origin_x = 0, origin_y = 0;
    u32 width = 0, height = 0;
    std::vector<u32> cells; // gids with flip flags, row-major; 0 = empty
    // Objects
    std::vector<MapObject> objects;
    // Image
    std::string image_path;
    Asset<Texture> image;

    // Gid at a tile coordinate (0 outside the layer).
    u32 at(i32 x, i32 y) const;
};

class Tilemap {
public:
    enum class Orientation : u8 { Orthogonal, Isometric };

    Orientation orientation = Orientation::Orthogonal;
    u32 width = 0, height = 0;           // in tiles (finite maps)
    u32 tile_width = 0, tile_height = 0; // grid cell size in pixels
    bool infinite = false;
    Color background = Color::transparent();
    std::vector<Tileset> tilesets;
    std::vector<MapLayer> layers;
    json::Value properties;

    // Reads a .tmj document. `read_file` resolves paths relative to `base_dir` (external .tsj tilesets).
    // Textures aren't loaded here; the asset loader fills them in.
    static bool parse(const json::Value& doc, std::string_view base_dir,
                      const std::function<std::optional<std::string>(const std::string& path)>& read_file,
                      Tilemap& out, std::string& error);

    const MapLayer* layer(std::string_view name) const;
    // The tileset owning a gid (flip flags ignored), or null.
    const Tileset* tileset_for(u32 gid) const;
    const TileInfo* tile_info(u32 gid) const;
    // Custom property of the tile at a cell, e.g. map.tile_property("ground", x, y, "solid").as_bool()
    const json::Value& tile_property(std::string_view layer_name, i32 x, i32 y, std::string_view key) const;

    // Map pixel coordinates of a cell's anchor: its top-left (orthogonal) or the top corner of the diamond
    // (isometric) — where Tiled puts tile (x, y).
    Vec2 cell_to_world(f32 x, f32 y) const;
    // Inverse: fractional cell coordinates of a map point; floor() them for the cell.
    Vec2 world_to_cell(Vec2 world) const;
};

struct TilemapDrawOptions {
    Vec2 position;        // where the map's origin goes in the world
    f32 time = 0;         // seconds, for animated tiles
    f32 depth = 0;        // base depth; each layer adds layer_depth_step
    f32 layer_depth_step = 1;
    // Depth = tile bottom y (+ base), so sprites at depth = their feet's y walk behind tall tiles (isometric).
    // Which layers sort: those with a Tiled bool property "ysort"; if no layer has it, every layer but the first.
    // Layers that don't sort draw underneath everything sorted (ground).
    bool y_sort = false;
    std::string_view only_layer; // draw just this layer
};

// Draws the visible part of the map (culled against the batch camera's view).
void draw_tilemap(SpriteBatch& batch, const Tilemap& map, const TilemapDrawOptions& options = {});

void register_tilemap_loader(AssetManager& assets);

// "maps/../tiles/a.png" -> "tiles/a.png"
std::string normalize_path(std::string_view path);

} // namespace ph::gfx
