# Tilemaps

Maps are made in [Tiled](https://www.mapeditor.org) and loaded with their tilesets, images, objects and custom
properties. Orthogonal and isometric (diamond) maps are supported, finite or infinite, with animated tiles, flipped
tiles and y-sorting for isometric depth.

<p align="center"><img src="../images/isometric-day.jpg" width="70%" alt="An isometric map with y-sorted pillars"></p>

- [Saving maps from Tiled](#saving-maps-from-tiled)
- [Loading and drawing](#loading-and-drawing)
- [Layers](#layers)
- [Coordinates](#coordinates)
- [Isometric maps and y-sorting](#isometric-maps-and-y-sorting)
- [Custom properties](#custom-properties)
- [Objects](#objects)
- [Collision with tiles](#collision-with-tiles)
- [Maps built in code](#maps-built-in-code)
- [What's supported](#whats-supported)

## Saving maps from Tiled

- Save maps as **JSON** (`.tmj`). External tilesets as JSON too (`.tsj`); embedded tilesets also work.
- Layer data format: **CSV**, **Base64 (uncompressed)**, **Base64 (zlib)** or **Base64 (gzip)**. Zstandard isn't
  supported; Tiled's default (CSV) is fine.
- Keep image paths relative (Tiled's default), and keep everything under your assets folder.

## Loading and drawing

```cpp
#include "ph/gfx/tilemap.hpp"

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
with `assets.add`, which is how the isometric sample generates its island. `Tilemap::parse` reads `.tmj` JSON you've
loaded some other way.

## What's supported

| Feature | |
| --- | --- |
| Orientation | Orthogonal, isometric. Not yet: staggered, hexagonal |
| Map format | JSON (`.tmj`, `.tsj`). Not yet: XML (`.tmx`, `.tsx`) |
| Layer data | CSV, base64, base64 + zlib, base64 + gzip |
| Infinite maps | Yes; chunks are merged into one rectangle per layer |
| Tilesets | Image tilesets (margin, spacing, tile offset), image-collection tilesets, external and embedded |
| Tiles | Flips (horizontal, vertical, diagonal), animation, properties, class |
| Layers | Tile, object, image, group (flattened); visibility, opacity, tint, offset |
| Objects | All shapes; tile objects are drawn |
