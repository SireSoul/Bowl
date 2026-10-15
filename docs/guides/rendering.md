# Rendering

Everything on screen is a sprite: a textured, colored, rotated rectangle. Sprites are collected in batches, and the
renderer draws the batches with post-processing and 2D lighting on top. This page covers all of it.

- [Sprite batches](#sprite-batches)
- [Cameras](#cameras)
- [Sorting and depth](#sorting-and-depth)
- [Color and blending](#color-and-blending)
- [Textures](#textures)
- [Materials](#materials)
- [The three layers: scene, lights, UI](#the-three-layers-scene-lights-ui)
- [Post-processing](#post-processing)
- [2D lighting](#2d-lighting)
- [Pixel art](#pixel-art)
- [Screenshots and frame capture](#screenshots-and-frame-capture)
- [Stats](#stats)

## Sprite batches

```cpp
gfx::SpriteBatch batch; // keep it as a member; it reuses its memory

void render(Engine& engine) override {
    batch.begin(engine.camera());
    batch.draw(*tree, {120, 80});                                  // a texture at its pixel size, centered there
    batch.draw_rect({0, 0, 64, 16}, Color::hex(0x3fa34d));         // a filled rectangle
    engine.renderer().submit(batch);
}
```

`begin` clears the batch and sets its camera, sort order, blend mode and optional material. Fill it, then submit it;
the renderer copies what it needs, so the batch can be refilled right away. Submit as many batches as you like each
frame; they're drawn in the order submitted.

Drawing calls:

| Call | Draws |
| --- | --- |
| `draw(texture, sprite)` | A full `SpriteInstance` (below) |
| `draw(texture, position, color, depth)` | A texture at its pixel size, centered on `position` |
| `draw_region(texture, source, position, size, color, depth, rotation, origin)` | Part of a texture (source rectangle in pixels), e.g. an atlas frame |
| `draw_rect(rect, color, depth)` | A filled rectangle, top-left at `rect.x, rect.y` |
| `draw_outline(rect, color, thickness, depth)` | A rectangle outline drawn inside `rect` |
| `draw_nine_slice(texture, dest, insets, color, depth)` | A panel whose corners keep their size while the edges and middle stretch |

A `SpriteInstance` is exactly what the GPU receives, 64 bytes:

```cpp
gfx::SpriteInstance s;
s.position = {100, 50};      // where the origin goes
s.size = {32, 48};           // world units
s.origin = {0.5f, 1.0f};     // pivot within the sprite (0..1): here, bottom center ("feet")
s.rotation = 0.3f;           // radians, around the origin
s.depth = s.position.y;      // draw order
s.uv = {0, 0, 1, 1};         // texture coordinates: u0, v0, u1, v1 (swap u0/u1 to mirror)
s.color = Color::white();    // multiplies the texture
batch.draw(texture, s);
```

C++20 designated initializers make one-liners easy: `batch.draw(glow, {.position = p, .size = {12, 12}})`.

Text, sprite sheets, tilemaps and particles all draw into batches too: see [text.md](text.md),
[sprite-animation.md](sprite-animation.md), [tilemaps.md](tilemaps.md) and
[particles-and-tweens.md](particles-and-tweens.md).

## Cameras

A `gfx::Camera2D` maps world coordinates to the screen. y points down.

```cpp
gfx::Camera2D& cam = engine.camera();     // the engine's main camera; its viewport follows the window
cam.position = player_position;            // the world point at the center of the screen
cam.zoom = 2.0f;                           // 2 = everything twice as big
cam.rotation = 0;                          // radians

Vec2 world = cam.screen_to_world(engine.input().mouse_position());
Vec2 screen = cam.world_to_screen(world);
Rect visible = cam.visible_bounds();       // for culling your own drawing
```

For HUD and menus, use a screen camera, where world units are logical pixels with the origin at the top left:

```cpp
hud.begin(gfx::Camera2D::screen(engine.window().size()), gfx::SpriteSort::None);
```

Each batch has its own camera, so a split screen or a minimap is just another batch with another camera.

Smooth following is a lerp in `update`: `cam.position = lerp(cam.position, target, 0.15f);`.

## Sorting and depth

Each batch sorts its sprites before drawing:

| `SpriteSort` | Order |
| --- | --- |
| `Depth` (default) | By `depth`, lowest first; equal depths are grouped by texture |
| `Texture` | Grouped by texture for the fewest draw calls; no order guarantees between textures |
| `None` | Exactly the order you drew them |

For top-down and isometric games, set each sprite's depth to the y of its feet: things lower on screen draw in front.
Give the ground a depth below anything else (or put it in its own earlier batch). For UI, use `None`.

Sorting happens per batch. Batches always draw in submission order.

## Color and blending

Colors are `ph::Color`, four floats:

```cpp
Color a = Color::hex(0xffcf5c);            // from 0xRRGGBB
Color b = Color::hex(0xffcf5c, 0.5f);      // with alpha
Color c = Color::rgba8(255, 207, 92);      // from bytes
Color d = a.with_alpha(0.2f);
Color e = lerp(a, b, 0.5f);
Color glow{3.0f, 2.0f, 1.0f, 1};           // over-bright: blooms (see below)
```

Everything is drawn with premultiplied alpha; textures are converted on load and the sprite shader premultiplies
vertex colors, so you write normal colors and get correct edges.

Each batch has a blend mode, set in `begin`:

| `BlendMode` | Use for |
| --- | --- |
| `Alpha` (default) | Normal transparency |
| `Additive` | Glows, fire, sparks, lights |
| `Multiply` | Shadows, darkening, tinting what's underneath |
| `Opaque` | Overwriting, for backgrounds |

## Textures

Load textures as assets (PNG, JPG, TGA, BMP):

```cpp
Asset<gfx::Texture> hero = engine.assets().load<gfx::Texture>("characters/hero.png");
```

They hot-reload when the file changes. Linear-filtered textures get mipmaps, so they look right when zoomed out.

Make textures in code with the GPU device:

```cpp
std::vector<u8> pixels(w * h * 4); // RGBA8, straight alpha
// ... fill
auto texture = engine.gpu().create_texture({.width = w, .height = h, .rgba8 = pixels.data(), .name = "noise"});
Asset<gfx::Texture> noise = engine.assets().add("generated/noise", std::move(texture));
```

`engine.gpu().update_texture(texture, pixels, premultiplied)` replaces all of a texture's pixels, and
`update_texture_region` a rectangle of them.

The renderer provides two built-in textures: `renderer.white_texture()` (1x1 white, what `draw_rect` uses) and
`renderer.light_texture()` (a soft radial falloff, for lights, glows and particles).

## Materials

A material swaps the sprite fragment shader for your own, for a whole batch:

```cpp
gfx::Material hologram;
hologram.fragment = engine.assets().load<gfx::Shader>("shaders/hologram.frag"); // assets/shaders/hologram.frag.glsl

struct HologramParams {
    f32 time, pad[3]; // std140: pad to 16 bytes
};
hologram.set_uniforms(HologramParams{static_cast<f32>(engine.time()), {}});

batch.begin(engine.camera(), gfx::SpriteSort::Depth, gfx::BlendMode::Alpha, &hologram);
```

The shader receives the sprite shader's outputs and resources:

```glsl
#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;                      // premultiplied
layout(location = 0) out vec4 out_color;                   // premultiplied

layout(set = 2, binding = 0) uniform sampler2D u_texture;  // the sprite's texture
layout(set = 2, binding = 1) uniform sampler2D u_extra0;   // material.textures[0] (white if unset)
layout(std140, set = 3, binding = 0) uniform Params { vec4 time; };

void main() {
    out_color = texture(u_texture, v_uv) * v_color;
}
```

- Up to three extra textures (`material.textures`, bindings 1 to 3).
- Up to 256 bytes of uniforms, laid out as std140; `set_uniforms` checks the size is a multiple of 16.
- The material must outlive the frame it's submitted in; keep it as a member.

The sprites sample's [hologram shader](../../samples/sprites/assets/shaders/hologram.frag.glsl) is a complete
example. Writing shaders is covered in [shaders.md](shaders.md).

## The three layers: scene, lights, UI

The renderer has three kinds of submission, drawn in this order:

```
submit(batch)        the scene, rendered in HDR (16-bit float) so colors can exceed 1
submit_light(batch)  the light map: half resolution, always additive, only used with post().lighting
                     ... post-processing combines them: lighting, bloom, grading, vignette ...
submit_ui(batch)     the UI, drawn on the finished image: not lit, not bloomed, not graded
```

Use `submit_ui` for HUD and menus so they stay readable whatever the scene looks like. The F1 overlay draws on top
of everything.

## Post-processing

`engine.renderer().post()` returns the `PostSettings` used for every frame. The defaults change nothing, so each
effect costs nothing until you turn it on.

```cpp
gfx::PostSettings& post = engine.renderer().post();
post.bloom = true;
post.vignette = 0.4f;
post.saturation = 0.9f;
```

| Setting | Default | Effect |
| --- | --- | --- |
| `exposure` | 1 | Brightness multiplier |
| `contrast` | 1 | Around mid-gray |
| `saturation` | 1 | 0 is grayscale |
| `tint` | white | Multiplies the image |
| `tonemap` | `false` | ACES-fitted tonemapping, for scenes with a lot of bright, additive light |
| `bloom` | `false` | Pixels brighter than the threshold glow |
| `bloom_threshold` | 0.9 | Brightness where bloom starts |
| `bloom_knee` | 0.4 | Softness of that start |
| `bloom_intensity` | 0.7 | Strength |
| `bloom_radius` | 1 | Spread |
| `vignette` | 0 | Edge darkening, 0..1 |
| `vignette_radius`, `vignette_softness` | 0.55, 0.6 | Its shape |
| `lighting` | `false` | Multiply the scene by ambient + light map |
| `ambient` | dark blue-gray | Light everywhere, before lights are added |

Bloom works on brightness above the threshold, and since the scene is HDR, a sprite color like
`{3.0f, 2.0f, 1.0f, 1}` is three times brighter than white in red and glows strongly. That's how the samples make
lanterns, fireflies and sparks glow without touching the rest of the image.

Every value can be tuned live in the overlay's **Post & lighting** panel. `renderer.set_clear_color(color)` sets the
background.

## 2D lighting

With `post().lighting` on, the final color of each scene pixel is `scene * (ambient + light)`. You draw lights as
sprites into the light map with `submit_light`; light batches always blend additively.

```cpp
post.lighting = true;
post.ambient = {0.16f, 0.2f, 0.38f, 1}; // moonlight

lights.begin(engine.camera(), gfx::SpriteSort::None, gfx::BlendMode::Additive);
lights.draw(renderer.light_texture(), {.position = torch, .size = {400, 300}, .color = {0.8f, 0.6f, 0.4f, 1}});
renderer.submit_light(lights);
```

- The light map is half resolution, which is invisible for soft lights and halves their cost.
- Any texture works as a light: cones, window shapes, flickering sprite sheets.
- Light colors add up, and `ambient + light` can go above 1, which brightens the scene past its original colors.
- UI isn't lit.

The isometric sample's night mode (`--night`) puts a lantern on every pillar and a torch on the player; read its
`render` for a complete setup.

## Pixel art

Set `AppConfig::pixel_art = true` and textures load with nearest filtering and no mipmaps, and the main camera snaps
to whole pixels. Use integer zoom levels for perfectly square pixels.

## Screenshots and frame capture

`--screenshot file.png` writes the frame shown at frame 60, or at the last frame when combined with `--frames N`.

From code, `renderer.capture_next_frame(callback)` hands you the next frame's pixels (RGBA8, top row first,
overlays included); `gfx::write_png(path, width, height, pixels)` saves them. Capturing stalls that one frame while
the GPU finishes.

## Stats

`renderer.stats()` describes the last finished frame: `sprites`, `draw_calls`, `batches` and `pipelines`. The overlay
shows them under the frame graph. Draw calls go up with each texture change within a batch (when sorting can't group
them), with each batch, and with each material. See [performance.md](performance.md).
