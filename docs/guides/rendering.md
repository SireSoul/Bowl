# Rendering

Everything on screen is a sprite: a textured, colored, rotated rectangle. Sprites are collected in batches, and the
renderer draws the batches with post-processing and 2D lighting on top. This page covers all of it.

- [Sprite batches](#sprite-batches)
- [Cameras](#cameras)
- [Sorting and depth](#sorting-and-depth)
- [Color and blending](#color-and-blending)
- [Textures](#textures)
- [Materials](#materials)
- [Material files](#material-files)
- [The layers: scene, lights, UI](#the-layers-scene-lights-ui)
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

Colors are `bowl::Color`, four floats:

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
- Up to 256 bytes of uniforms, laid out as std140; `set_uniforms` checks the size is a multiple of 16. What they don't
  cover takes the defaults the shader's comments give, and `bowl_time` and `bowl_screen` are filled in for you (see
  [shaders.md](shaders.md#parameters-and-what-the-editor-shows)).
- `blend`, when set, replaces the batch's blend mode for the material's sprites.
- The material must outlive the frame it's submitted in; keep it as a member.

`batch.set_filter(gfx::Filter::Nearest)` samples every texture in a batch (and its material's) with that filter
instead of each texture's own, until it's changed: small pixel art stays sharp in a zoomed-in view of a game that
otherwise smooths its textures.

A material can also be given to single sprites, so they keep their place in the batch's depth order:
`batch.draw(texture, sprite, &material)`. Sprites at the same depth are grouped by material, and each change of material
is a draw call.

The sprites sample's [hologram shader](../../samples/sprites/assets/shaders/hologram.frag.glsl) is a complete
example. Writing shaders is covered in [shaders.md](shaders.md).

## Material files

[Materials and shaders](materials.md) walks through making them in Bowl Editor and using them in a game.

A `.material` file is a sprite's whole look as an asset: a shader with its parameters and textures, a glow (an emissive
map), a normal map and a shadow. Bowl Editor's Materials workspace makes and edits them (and makes the maps); a game
loads them:

```json
{
  "shader": "shaders/water.frag",
  "params": {"speed": 1.5, "tint": [0.4, 0.8, 1, 1]},
  "textures": {"u_noise": "textures/noise.png"},
  "blend": "alpha",
  "emissive": {"map": "props/lamp_emissive.png", "color": "#ffd890", "strength": 3},
  "normal": "props/lamp_normal.png",
  "shadow": "shape"
}
```

| Key | |
| --- | --- |
| `shader` | Its own fragment shader; none: the sprite shader |
| `params` | The shader's parameters by name: a number, a list for a vector or color, `"#rrggbb"`, or `true`/`false`. Missing ones take the shader's defaults |
| `textures` | The shader's extra textures, by the names the shader gives them |
| `blend` | `alpha`, `additive`, `multiply` or `opaque` |
| `emissive` | Its glow: `map` (bright where it glows, black or see-through where not; none: the picture glows where it's bright), `color` and `strength`. Just a path is a map at strength 1 |
| `normal` | Its normal map, for lights drawn with `light_material` |
| `shadow` | `none`, `shape` (blocks light) or `faint` (blocks nothing and is never shadowed) |
| `preview` | The picture the editor shows it on; games ignore it |

The maps line up with the sprite's texture (the same layout, at any size), so sprite sheet frames and atlas regions find
their parts. A `gfx::Sprite` takes one in `material` (and a level's Art card sets it), and it hot-reloads, with the
shader and textures it names:

```cpp
Asset<gfx::SpriteMaterial> lamp = engine.assets().load<gfx::SpriteMaterial>("props/lamp.material");
world.create(ecs::Transform2D{{40, 80}}, gfx::Sprite{.texture = lamp_png, .material = lamp});
```

`draw_sprites(world, batch)` draws each sprite with its material's shader. Its glow, normal map and shadow each go to
their own layer of the renderer, so draw through a `gfx::SpriteLayers` to get them too:

```cpp
gfx::SpriteLayers layers; // a member: it reuses its memory

layers.begin(engine.camera());
gfx::draw_maps(world, layers.scene, static_cast<f32>(engine.time()));
gfx::draw_sprites(world, layers);
layers.submit(engine.renderer());   // scene, emissive, normals, occluders
```

Drawing your own way, `layers.draw(texture, sprite, material.get())` does the same for one sprite, and
`material->look()` is the `gfx::Material` for its shader (null when it draws as a plain sprite). The glow shows with or
without lighting and blooms with `post().bloom`; normal maps and shadows show under lights drawn with `light_material`.
`material->set_param("speed", values)` changes a parameter from code for the next draw (on every sprite using the
material).

## The layers: scene, lights, UI

The renderer has these kinds of submission:

```
submit(batch)            the scene, rendered in HDR (16-bit float) so colors can exceed 1
submit_emissive(batch)   sprites that light themselves: over the lit scene, unaffected by the light map
submit_light(batch)      the light map: half resolution, always additive, only used with post().lighting
submit_normals(batch)    normal maps, for lights drawn with light_material
submit_occluders(batch)  shadow casters, for lights drawn with light_material
                         ... then lighting, your scene effects, bloom, grading, LUT, lens, vignette,
                             your final effects ...
submit_ui(batch)         the UI, drawn on the finished image: not lit, not bloomed, not graded
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
| `lut`, `lut_strength` | none, 1 | A color-grading lookup table, see below |
| `chromatic_aberration` | 0 | Red and blue split toward the edges: 0.002 is subtle, 0.01 strong |
| `grain` | 0 | Moving film grain, strongest in the midtones: 0.02 to 0.1 |

Bloom works on brightness above the threshold, and since the scene is HDR, a sprite color like
`{3.0f, 2.0f, 1.0f, 1}` is three times brighter than white in red and glows strongly. That's how the samples make
lanterns, fireflies and sparks glow without touching the rest of the image.

Every value can be tuned live in the overlay's **Post & lighting** panel. `renderer.set_clear_color(color)` sets the
background.

### Color grading with a lookup table

A LUT lets you grade the game in any image editor. Export the neutral table, take a screenshot of the game, grade the
screenshot with adjustment layers, apply the same layers to the table, and load the result:

```cpp
// once, to make the starting file
std::vector<u8> neutral = gfx::make_neutral_lut(16);
gfx::write_png("lut_neutral.png", 16 * 16, 16, neutral);

// in the game
Asset<gfx::Texture> dusk = engine.assets().load<gfx::Texture>("luts/dusk.png");
post.lut = dusk.get();
post.lut_strength = 0.8f;   // blend with the ungraded image
```

The table is a strip of N slices, each N x N (red across, green down, blue from slice to slice), the layout most
grading tools export; 16 or 32 slices both work. It's applied after grading and tonemapping. Load LUT textures
without pixel-art filtering (they're always sampled smoothly). Swap or blend tables to change the mood: dusk, a
flashback, a poisoned haze.

### Your own effects

Add full-screen passes of your own with `post_effects()`. Each is a fragment shader in a `Material`:

```cpp
gfx::Material scanlines;
scanlines.fragment = engine.assets().load<gfx::Shader>("shaders/scanlines.frag");
scanlines.set_uniforms(Params{0.18f, 360});
engine.renderer().post_effects().push_back({&scanlines, gfx::PostStage::Final});
```

```glsl
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_image;   // the image so far
layout(std140, set = 3, binding = 0) uniform Params { vec4 params; };

void main() {
    vec3 color = texture(u_image, v_uv).rgb;
    out_color = vec4(color * (1.0 - params.x * (0.5 + 0.5 * sin(v_uv.y * params.y * 6.2831853))), 1.0);
}
```

- `PostStage::Scene` effects run on the lit HDR scene before bloom: heat haze, underwater wobble, shockwaves.
  `PostStage::Final` effects run on the finished, graded image before the UI: pixelation, CRT lines, dithering.
- Effects run in the order they're in the list; set `enabled = false` to skip one. The material's textures are at
  bindings 1-3 and its uniforms at set 3 binding 0; `textureSize(u_image, 0)` gives the image size. Pass time
  through your uniforms if the effect moves.
- The material is kept by pointer, so keep it alive while it's in the list.

The sprites sample's `--film` flag uses a LUT, grain, chromatic aberration, a vignette and a scanline effect.

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
- Bloom comes from the scene's own bright pixels (over-bright sprites, emissive ones), not from where lights overlap.

The isometric sample's night mode (`--night`) puts a lantern on every pillar and a torch that casts shadows on the
player; read its `render` for a complete setup.

### Emissive sprites

Things that give off their own light (lamps, glowing eyes, screens, lava) shouldn't darken at night. Draw them with
`submit_emissive`: they appear over the lit scene at full brightness, and bloom like any bright sprite.

```cpp
glowing.begin(engine.camera());
glowing.draw(lamp_texture, lamp);
engine.renderer().submit_emissive(glowing);
```

Emissive sprites lie over everything in the scene, so use them for things on top, or draw just the glowing part
(the flame, not the lantern).

### Normal maps and shadows

Lights drawn with `gfx::light_material` go further: they shade normal-mapped sprites and cast shadows.

```cpp
// once
gfx::Material light_look = gfx::light_material(engine.assets(), {.normals = true, .height = 0.5f, .shadows = true});

// every frame
normals.begin(engine.camera());
normals.draw(rock_normal_map, rock);          // the same sprite as in the scene, with its normal map
engine.renderer().submit_normals(normals);

occluders.begin(engine.camera());
occluders.draw_rect(wall_rect, Color::white());   // blocks light
engine.renderer().submit_occluders(occluders);

lights.begin(engine.camera(), gfx::SpriteSort::None, gfx::BlendMode::Additive, &light_look);
lights.draw(renderer.light_texture(), torch);
engine.renderer().submit_light(lights);
```

- **Normal maps** use the OpenGL convention (green points up), what most tools make by default; flip the green
  channel of DirectX-style maps. Unbumped areas look exactly as they would without normal mapping. `height` is how
  far above the ground lights hang, relative to their radius: low lights rake across bumps, high ones light them
  flat. Sprite rotation and flips aren't taken into account.
- **Occluders** block light where their alpha is above one half; the light shader traces each pixel's way back to
  the light. Anything drawn faintly (alpha around a quarter) blocks nothing but is never shadowed itself. In
  isometric and side views, mark the upright parts of things that way and draw only their footprint at full alpha,
  so the shadow falls behind them and not over them; the isometric sample does exactly that for its pillars.
- Lights keep their shapes and colors; any light texture works. Lights without the material are drawn as before.
- With nothing submitted to `submit_normals` or `submit_occluders`, those parts cost nothing.

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
