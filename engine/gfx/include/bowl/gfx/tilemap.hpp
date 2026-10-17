#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/core/json.hpp"
#include "bowl/ecs/hierarchy.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/gfx/sprites.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace bowl::gfx {

// Tiled maps (mapeditor.org), loaded from Tiled's JSON (.tmj, .tsj) or XML (.tmx, .tsx) files. Orthogonal, isometric
// (diamond), staggered and hexagonal orientations; finite and infinite maps; CSV, XML and base64 (+zlib/gzip) data.

inline constexpr u32 tile_flip_h = 0x80000000u;
inline constexpr u32 tile_flip_v = 0x40000000u;
inline constexpr u32 tile_flip_d = 0x20000000u; // anti-diagonal (transpose)
inline constexpr u32 tile_gid_mask = 0x0FFFFFFFu;
// The most cells a tile layer can have (4096 x 4096, 64 MB). A map with a bigger one fails to load, so a broken or
// hostile map file can't make a game ask for gigabytes.
inline constexpr u64 max_layer_cells = u64{1} << 24;

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

// A Tiled terrain set ("Wang set"): which tile fits which terrains at its corners and edges. Kept as data, for drawing
// terrain from code (procedural maps, digging, farm plots): see corner_tile and fill_from_corners.
struct WangSet {
    enum class Type : u8 { Corner, Edge, Mixed };
    struct Terrain {
        std::string name;
        Color color;
        f32 probability = 1;
        json::Value properties;
    };
    struct Tile {
        u32 tile; // local id in the tileset
        // Terrain at each spot, 1-based into `terrains` (0: none), going round from the top:
        // top, top-right, right, bottom-right, bottom, bottom-left, left, top-left. Corners are 1, 3, 5, 7.
        std::array<u8, 8> wang{};
    };
    std::string name;
    Type type = Type::Corner;
    std::vector<Terrain> terrains;
    std::vector<Tile> tiles;
    json::Value properties;

    // Terrain number by name (1-based), or 0.
    u8 terrain(std::string_view terrain_name) const;
    // Local ids of tiles whose terrains match `want` (a 0 in `want` matches anything).
    std::vector<u32> matching(const std::array<u8, 8>& want) const;
    // A tile with these terrains at its corners (top-left, top-right, bottom-right, bottom-left), choosing among
    // variants by `pick` (any number: a hash of the cell keeps choices stable). Nothing if no tile fits.
    std::optional<u32> corner_tile(u8 top_left, u8 top_right, u8 bottom_right, u8 bottom_left, u32 pick = 0) const;
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
    std::vector<WangSet> wang_sets;
    json::Value properties;
    std::string source; // the external tileset file it came from (normalized virtual path), if any

    const TileInfo* tile(u32 local_id) const;
    Rect source_rect(u32 local_id) const; // pixels in `image`
    const WangSet* wang_set(std::string_view set_name) const;
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
    enum class Orientation : u8 { Orthogonal, Isometric, Staggered, Hexagonal };

    Orientation orientation = Orientation::Orthogonal;
    // Staggered and hexagonal maps: which axis is shifted every other row or column, whether the shifted ones are the
    // even or the odd ones, and (hexagonal) the length of the hexagon's flat side in pixels.
    bool stagger_x = false;
    bool stagger_even = false;
    u32 hex_side_length = 0;
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
    // Reads a .tmx document (XML). External tilesets may be .tsx or .tsj.
    static bool parse_tmx(std::string_view text, std::string_view base_dir,
                          const std::function<std::optional<std::string>(const std::string& path)>& read_file,
                          Tilemap& out, std::string& error);

    const MapLayer* layer(std::string_view name) const;
    // The tileset owning a gid (flip flags ignored), or null.
    const Tileset* tileset_for(u32 gid) const;
    const TileInfo* tile_info(u32 gid) const;
    // Custom property of the tile at a cell, e.g. map.tile_property("ground", x, y, "solid").as_bool()
    const json::Value& tile_property(std::string_view layer_name, i32 x, i32 y, std::string_view key) const;

    // Map pixel coordinates of a cell's anchor, where Tiled puts tile (x, y): its top-left (orthogonal, and the top-left
    // of the tile's box for staggered and hexagonal maps) or the top corner of the diamond (isometric).
    Vec2 cell_to_world(f32 x, f32 y) const;
    // Inverse: fractional cell coordinates of a map point; floor() them for the cell. Staggered and hexagonal maps
    // return the whole cell under the point.
    Vec2 world_to_cell(Vec2 world) const;
    // The y a tile in cell (x, y) sorts by when y-sorted: the middle of the cell, where the feet of a sprite standing
    // in the middle of it would be. Sprites sort by their feet, so they go behind a tile from the far sides and in
    // front from the near ones.
    f32 sort_y(i32 x, i32 y) const;
    // The cell under a map point.
    std::pair<i32, i32> cell_at(Vec2 world) const;
    // Size of the whole map in pixels (finite maps).
    Vec2 pixel_size() const;
};

struct TilemapDrawOptions {
    Vec2 position;        // where the map's origin goes in the world
    f32 time = 0;         // seconds, for animated tiles
    f32 depth = 0;        // base depth; each layer adds layer_depth_step
    f32 layer_depth_step = 1;
    // Depth = the middle of the tile's cell (Tilemap::sort_y, + base), so sprites at depth = their feet's y go behind
    // tall tiles from the far sides and in front from the near ones (isometric).
    // Which layers sort: those with a Tiled bool property "ysort"; if no layer has it, every layer but the first.
    // Layers that don't sort draw underneath everything sorted (ground).
    bool y_sort = false;
    std::string_view only_layer; // draw just this layer
    // Which layers to draw: all, only the flat ones (ground) or only the y-sorted ones. TilemapCache draws the flat
    // ones; pass Sorted to draw the rest each frame.
    enum class Layers : u8 { All, Flat, Sorted };
    Layers layers = Layers::All;
};

// Draws the visible part of the map (culled against the batch camera's view).
void draw_tilemap(SpriteBatch& batch, const Tilemap& map, const TilemapDrawOptions& options = {});

// Fills a tile layer from terrain at the corners of its cells: `corners` is (width + 1) x (height + 1) terrain numbers
// of `set` (row by row), and every cell gets a tile that fits its four corners, chosen among variants by position so
// the same corners always give the same map. The layer becomes width x height at its origin. Returns how many cells
// had no fitting tile (left empty).
usize fill_from_corners(MapLayer& layer, const Tileset& tileset, const WangSet& set, std::span<const u8> corners,
                        u32 width, u32 height);

// Draws large maps cheaply: the flat layers are cut into chunks of chunk_cells x chunk_cells tiles, each built once into
// StaticSprites in GPU memory, so a frame only submits the chunks on screen. Animated tiles are still drawn every
// frame. Y-sorted layers aren't cached: draw them with draw_tilemap and `.layers = Layers::Sorted`.
//
// Chunks rebuild by themselves when the map asset or its textures reload; call invalidate() after changing cells.
class TilemapCache : NonCopyable {
public:
    static constexpr i32 chunk_cells = 32;

    // Submits the visible part of the map's flat layers to the renderer, as seen through `camera`, in layer order.
    void submit(Renderer& renderer, GpuDevice& device, const Camera2D& camera, const Tilemap& map,
                const TilemapDrawOptions& options = {});
    void invalidate();
    usize chunk_count() const { return chunks_.size(); }

private:
    struct Chunk {
        usize layer;
        StaticSprites sprites;
    };
    struct Animated {
        usize layer;
        i32 x, y;
    };
    void build(GpuDevice& device, const Tilemap& map, const TilemapDrawOptions& options);
    u64 signature(const Tilemap& map, const TilemapDrawOptions& options) const;

    u64 built_for_ = 0;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    std::vector<Animated> animated_;
    SpriteBatch scratch_, moving_;
};

void register_tilemap_loader(AssetManager& assets);

// "maps/../tiles/a.png" -> "tiles/a.png"
std::string normalize_path(std::string_view path);

// A Tiled map placed in the world, at its entity's position (GlobalTransform2D; maps don't turn or scale). It's what
// Bowl Editor puts in a level for a .tmj or .tmx file, and draw_maps draws every one.
struct Map {
    Asset<Tilemap> map;
    f32 depth = 0;      // added to every tile's draw depth
    bool y_sort = true; // tall layers sort with sprites by the middles of their cells (TilemapDrawOptions::y_sort);
                        // off: all of it draws underneath sprites
    bool visible = true;
};
BOWL_COMPONENT(Map, map, depth, y_sort, visible)

// Draws every visible Map that has a GlobalTransform2D, into the same batch as the sprites so they sort together.
// `time` animates tiles; `skip` leaves entities out (Bowl Editor's hidden ones).
void draw_maps(ecs::World& world, SpriteBatch& batch, f32 time = 0,
               const std::function<bool(ecs::Entity)>& skip = {});

} // namespace bowl::gfx
