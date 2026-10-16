# Tilemaps

Maps are made in [Tiled](https://www.mapeditor.org) and loaded with their tilesets, images, objects and custom
properties. Orthogonal, isometric, staggered and hexagonal maps are supported, finite or infinite, saved as JSON or
XML, with animated tiles, flipped tiles, y-sorting for isometric depth, and GPU-cached chunks for very large maps.

<p align="center"><img src="../images/isometric-day.jpg" width="70%" alt="An isometric map with y-sorted pillars"></p>

- [Saving maps from Tiled](#saving-maps-from-tiled)
- [Loading and drawing](#loading-and-drawing)
- [Layers](#layers)
- [Coordinates](#coordinates)
- [Isometric maps and y-sorting](#isometric-maps-and-y-sorting)
- [Staggered and hexagonal maps](#staggered-and-hexagonal-maps)
- [Very large maps](#very-large-maps)
- [Custom properties](#custom-properties)
- [Objects](#objects)
- [Collision with tiles](#collision-with-tiles)
- [Maps built in code](#maps-built-in-code)
- [What's supported](#whats-supported)

## Saving maps from Tiled

- Save maps as **JSON** (`.tmj`) or **XML** (`.tmx`, Tiled's default). External tilesets can be either (`.tsj`,
  `.tsx`), and embedded tilesets work too. The file extension decides how a map is read.
- Layer data format: **CSV**, **XML**, **Base64 (uncompressed)**, **Base64 (zlib)** or **Base64 (gzip)**. Zstandard
  isn't supported; Tiled's default (CSV) is fine.
- Keep image paths relative (Tiled's default), and keep everything under your assets folder.

## Loading and drawing

```cpp
#include "bowl/gfx/tilemap.hpp"

Asset<gfx::Tilemap> town = engine.assets().load<gfx::Tilemap>("maps/town.tmj");

// render:
batch.begin(engine.camera());
gfx::draw_tilemap(batch, *town, {.time = static_cast<f32>(engine.time())});
engine.renderer().submit(batch);
```

`draw_tilemap` only draws the tiles the batch's camera can see, so big maps cost what's on screen.

Editing the map in Tiled and saving reloads it in the running game, along with changes to its tilesets and their
images.

`TilemapDrawOptions`:

| Field | Default | |
| --- | --- | --- |
| `position` | (0, 0) | Where the map's origin goes in the world |
| `time` | 0 | Seconds, for animated tiles; pass `engine.time()` |
| `depth` | 0 | Base depth for the map's sprites |
| `layer_depth_step` | 1 | Depth added per layer, so later layers draw on top |
| `y_sort` | `false` | Give tiles depth by their bottom edge (see below) |
| `only_layer` | empty | Draw just the layer with this name |
| `layers` | `All` | `Flat` or `Sorted` to draw only the layers that don't, or do, y-sort |

## Layers

```cpp
const gfx::MapLayer* ground = town->layer("ground");
u32 gid = ground->at(x, y) & gfx::tile_gid_mask;   // 0 means empty
```

Tile, object and image layers are loaded; group layers are flattened, with names joined as `"group/child"`. Each
layer keeps its visibility, opacity, tint, offset and custom properties. Invisible layers aren't drawn, but their data
is there, which makes hidden layers useful for gameplay data (spawn points, collision, regions).

Tile cells hold *gids*: a global tile id plus flip flags in the top bits. Mask with `gfx::tile_gid_mask` for the id;
`tile_flip_h`, `tile_flip_v` and `tile_flip_d` are the flags.

## Coordinates

```cpp
Vec2 world = map.cell_to_world(x, y);          // where Tiled places cell (x, y)
Vec2 cell = map.world_to_cell(world_point);    // fractional; floor() it for the cell
auto [cx, cy] = map.cell_at(world_point);      // the cell under a point, for every orientation
Vec2 size = map.pixel_size();                  // the whole map in pixels
```

For orthogonal maps a cell's anchor is its top-left corner. For isometric maps it's the top corner of the diamond,
with the map shifted right so its left corner is at x = 0, as in Tiled. Both are in map coordinates and ignore
`TilemapDrawOptions::position`; add it (or subtract it, going the other way) if you draw the map somewhere else.

In isometric maps, Tiled stores object positions along the two grid axes, with `tile_height` pixels per cell, and
`MapObject::bounds` keeps them as Tiled wrote them. To place an object in the world:

```cpp
f32 th = static_cast<f32>(map.tile_height);
Vec2 world = map.cell_to_world(object.bounds.x / th, object.bounds.y / th);
```

`draw_tilemap` does this for tile objects.

## Isometric maps and y-sorting

In an isometric map, tall tiles (walls, trees, pillars) must cover characters standing behind them and be covered by
characters in front. Turn on `y_sort` and give your sprites a depth equal to the y of their feet:

```cpp
gfx::draw_tilemap(batch, *map, {.time = t, .y_sort = true});

gfx::SpriteInstance s;
s.position = feet;
s.origin = {0.5f, 0.92f};   // pivot near the bottom of the sprite
s.depth = feet.y;
batch.draw(*player_texture, s);
```

With `y_sort`, tiles in sorted layers get a depth from their bottom edge. Which layers sort: any layer with a Tiled
bool property `ysort` set to true; if no layer has that property, every layer except the first. Layers that don't
sort (the ground) draw below everything that does.

The isometric sample shows the whole setup, including movement in screen directions and collision in cell space.
`--behind-pillar` starts the player behind a pillar to show the sorting.

## Staggered and hexagonal maps

Staggered maps are isometric diamonds laid out in rows, so the map is a rectangle; hexagonal maps are the same with
hexagons. Both shift every other row (stagger axis y) or column (stagger axis x), and Tiled's stagger axis, stagger
index and hex side length are kept as `stagger_x`, `stagger_even` and `hex_side_length`.

A cell's anchor (`cell_to_world`) is the top-left of the tile's box. `world_to_cell` and `cell_at` give the cell
whose hexagon or diamond contains the point, which is what you want for picking under the mouse. Object positions
are plain map pixels, as in orthogonal maps. Tiles draw in Tiled's order, so tall tiles overlap correctly, and
`y_sort` works as it does for isometric maps.

## Very large maps

`draw_tilemap` walks the visible cells every frame. For big maps zoomed out, that's a lot of cells. `TilemapCache`
cuts the flat layers into chunks of 32 x 32 cells and builds each chunk once into GPU memory
(`gfx::StaticSprites`); a frame only submits the chunks on screen:

```cpp
gfx::TilemapCache ground;   // keep it with the map

// render:
gfx::TilemapDrawOptions tiles{.time = t, .y_sort = true};
ground.submit(engine.renderer(), engine.gpu(), engine.camera(), *map, tiles);  // ground layers, cached
batch.begin(engine.camera());
tiles.layers = gfx::TilemapDrawOptions::Layers::Sorted;
gfx::draw_tilemap(batch, *map, tiles);                                         // walls and trees, each frame
// ... characters into the same batch ...
engine.renderer().submit(batch);
```

The cache draws the layers that don't y-sort (all of them when `y_sort` is off) in order, before anything submitted
after it. Animated tiles, image layers and tile objects in those layers are drawn every frame in between. Chunks
rebuild by themselves when the map or its textures reload, or when the draw options change; after changing cells in
code, call `invalidate()`.

`StaticSprites` works for anything else that doesn't change: fill a `SpriteBatch`, `build` from it once, and
`engine.renderer().submit(sprites, camera)` every frame.

## Custom properties

Tiled's custom properties are kept as JSON values on the map, tilesets, tiles, layers and objects:

```cpp
bool solid = map.tile_property("ground", x, y, "solid").as_bool();
f32 speed = map.properties["walk_speed"].as_float(1.0f);
std::string_view music = layer->properties["music"].as_string();
```

Missing properties read as null, so the fallback you pass to `as_bool`, `as_float` and friends is used.

## Objects

Object layers hold rectangles, points, ellipses, polygons, polylines, tile objects and text objects:

```cpp
for (const gfx::MapObject& o : map.layer("spawns")->objects) {
    if (o.type == "chest") spawn_chest(world, o.bounds.position(), o.properties);
}
```

Each object has `id`, `name`, `type` (Tiled's "class"), `shape`, `bounds` (position and size in map pixels),
`rotation` (degrees), `gid` for tile objects, `points` for polygons and polylines, `visible` and `properties`.

Tile objects are drawn by `draw_tilemap`; other shapes are data for your game to use.

### Object templates

Objects made from a Tiled template (`.tj` or `.tx`) are filled in from it when the map loads: the template gives the
shape, size, name, type and properties, and anything set on the object itself wins, property by property. A tile
template's tile is matched to the map's own copy of its tileset, so it draws the right tile whatever the tileset's
`firstgid` is in this map. Editing a template reloads every map that uses it.

## Terrain sets (Wang sets)

A tileset's terrain sets (what Tiled calls Wang sets) are kept as data, so code can draw terrain the way Tiled's
terrain brush does: for procedural maps, digging, tilled soil, flooding.

```cpp
const gfx::Tileset& ground = map.tilesets[0];
const gfx::WangSet* shore = ground.wang_set("shore");
u8 grass = shore->terrain("grass"), water = shore->terrain("water");

// One tile for four corners (top-left, top-right, bottom-right, bottom-left).
std::optional<u32> tile = shore->corner_tile(water, grass, grass, grass);

// A whole layer from terrain at the corners of its cells: (width + 1) x (height + 1) values, row by row.
std::vector<u8> corners = make_island(width + 1, height + 1);
gfx::fill_from_corners(layer, ground, *shore, corners, width, height);
```

Each `WangSet` has its `terrains` (name, color, probability, properties) and its `tiles` with their eight Wang ids.
`matching` finds every tile that fits a pattern, 0 meaning "any". When several tiles fit, `fill_from_corners`
chooses by position, so the same corners always give the same map. Cells with no terrain at any corner stay empty.
Corner sets are what `corner_tile` is for; edge and mixed sets are there as data.

## Collision with tiles

`physics::GridSolids` turns a predicate over cells into solid boxes for `move_and_slide`:

```cpp
auto solid = [&](i32 x, i32 y) { return map.tile_property("ground", x, y, "solid").as_bool(); };
Vec2 cell{static_cast<f32>(map.tile_width), static_cast<f32>(map.tile_height)};
physics::move_and_slide(box, delta, physics::GridSolids{cell, {0, 0}, solid});
```

For orthogonal maps, work in map pixels with the tile size as the cell size. For isometric maps, it's simpler to
move and collide in *cell* space (cell size 1) and convert to world coordinates only for drawing; the isometric sample
does exactly that. See [collision.md](collision.md).

## Maps built in code

A `gfx::Tilemap` is plain data. You can fill one yourself (orientation, tile size, tilesets, layers) and register it
with `assets.add`, which is how the isometric sample generates its island. `Tilemap::parse` reads `.tmj` JSON and
`Tilemap::parse_tmx` reads `.tmx` XML you've loaded some other way.

## What's supported

| Feature | |
| --- | --- |
| Orientation | Orthogonal, isometric, staggered, hexagonal (both stagger axes and indexes) |
| Map format | JSON (`.tmj`, `.tsj`) and XML (`.tmx`, `.tsx`) |
| Layer data | CSV, XML, base64, base64 + zlib, base64 + gzip |
| Infinite maps | Yes; chunks are merged into one rectangle per layer |
| Tilesets | Image tilesets (margin, spacing, tile offset), image-collection tilesets, external and embedded |
| Tiles | Flips (horizontal, vertical, diagonal), animation, properties, class |
| Layers | Tile, object, image, group (flattened); visibility, opacity, tint, offset |
| Objects | All shapes; tile objects are drawn; templates (`.tj`, `.tx`) |
| Terrain sets | Corner, edge and mixed Wang sets as data; `fill_from_corners` for corner sets |
