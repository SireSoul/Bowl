# gfx

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/gfx/bidi.hpp](#bowlgfxbidihpp)
- [bowl/gfx/bwtx.hpp](#bowlgfxbwtxhpp)
- [bowl/gfx/device.hpp](#bowlgfxdevicehpp)
- [bowl/gfx/gpu_particles.hpp](#bowlgfxgpu_particleshpp)
- [bowl/gfx/material.hpp](#bowlgfxmaterialhpp)
- [bowl/gfx/particles.hpp](#bowlgfxparticleshpp)
- [bowl/gfx/renderer.hpp](#bowlgfxrendererhpp)
- [bowl/gfx/sprite.hpp](#bowlgfxspritehpp)
- [bowl/gfx/sprite_sheet.hpp](#bowlgfxsprite_sheethpp)
- [bowl/gfx/sprites.hpp](#bowlgfxspriteshpp)
- [bowl/gfx/text.hpp](#bowlgfxtexthpp)
- [bowl/gfx/tilemap.hpp](#bowlgfxtilemaphpp)

## bowl/gfx/bidi.hpp

`#include "bowl/gfx/bidi.hpp"` · namespace `bowl::gfx::bidi`

```cpp
enum class Type : u8 { L, R, AL, EN, AN, ES, ET, CS, NSM, B, S, WS, ON };
Type type_of(u32 codepoint);
```

```cpp
enum class Direction : u8 { Auto, LeftToRight, RightToLeft };
```

### Paragraph

Embedding levels for one paragraph's characters: even runs left to right, odd right to left. Auto takes the
paragraph's direction from its first letter.

```cpp
struct Paragraph {
    u8 level = 0; // 0: left to right, 1: right to left
    std::vector<u8> levels;
};
```

```cpp
Paragraph resolve(std::span<const u32> codepoints, Direction direction = Direction::Auto);
```

The order to draw a line's characters in, as indexes into `levels` (a line's slice of a paragraph). Whitespace at
the end of the line goes to the paragraph's level first, so it stays at the end.

```cpp
std::vector<u32> visual_order(std::span<const u32> codepoints, std::span<const u8> levels, u8 paragraph_level);
```

The mirrored form of a bracket-like character, drawn in right-to-left runs: '(' for ')', and so on. Others are
returned as they are.

```cpp
u32 mirror(u32 codepoint);
```

## bowl/gfx/bwtx.hpp

`#include "bowl/gfx/bwtx.hpp"` · namespace `bowl::gfx`

BWTX: the picture format bowl-cook turns PNG and JPEG files into. RGBA8 pixels, losslessly packed so they decode
several times faster than a PNG while staying about its size. The texture loader (and decode_png) tells it apart by
its first four bytes, "BWTX", so a cooked picture keeps its .png name.

The layout: "BWTX", a version byte (1), the channel count (4), two zero bytes, the width and height as little-endian
u32s, then the pixels as a QOI stream (the "Quite OK Image" format, qoiformat.org) ending in seven 0 bytes and a 1.

```cpp
inline constexpr u32 bwtx_max_side = 16384; // as for every texture
```

```cpp
bool is_bwtx(std::span<const u8> bytes);
```

`rgba` holds width * height * 4 bytes, row by row from the top.

```cpp
std::vector<u8> encode_bwtx(u32 width, u32 height, std::span<const u8> rgba);
```

False for anything that isn't a whole BWTX picture of at most bwtx_max_side a side.

```cpp
bool decode_bwtx(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba);
```

## bowl/gfx/device.hpp

`#include "bowl/gfx/device.hpp"` · namespace `bowl::gfx`

```cpp
enum class Filter : u8 { Nearest, Linear };
```

### PixelRect

A rectangle of whole pixels in a texture, top-left origin.

```cpp
struct PixelRect {
    u32 x = 0, y = 0, w = 0, h = 0;
};
```

### TextureDesc

```cpp
struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    const void* rgba8 = nullptr; // width*height*4 bytes, straight alpha unless premultiplied is set
    bool premultiplied = false;
    Filter filter = Filter::Linear;
    bool mipmaps = true; // only with Linear filtering
    std::string_view name;
};
```

### Texture

```cpp
class Texture : NonCopyable
```

```cpp
~Texture();
SDL_GPUTexture* handle() const;
u32 width() const;
u32 height() const;
Vec2 size() const;
Filter filter() const;
```

Unique per texture object (never reused), for sorting draws.

```cpp
u32 id() const;
```

### Shader

One compiled shader stage on the GPU. Loaded as an asset from "&lt;path>" -> "&lt;path>.bowlsh" (or, in dev builds,
compiled live from "&lt;path>.glsl" so edits hot-reload).

```cpp
class Shader : NonCopyable
```

```cpp
~Shader();
SDL_GPUShader* handle() const;
ShaderStage stage() const;
u32 samplers() const;
u32 uniform_buffers() const;
```

Unique per shader object (never reused), for pipeline caches.

```cpp
u32 id() const;
```

The uniform block at binding 0 member by member, and the textures by binding, with what the shader's comments
say about them (see ShaderBundle::Uniform), so materials can set them by name.

```cpp
u32 block_size() const;
const std::vector<ShaderBundle::Uniform>& uniforms() const;
const std::vector<ShaderBundle::TextureSlot>& textures() const;
const ShaderBundle::Uniform* uniform(std::string_view name) const;
```

The block filled with the shader's own defaults (zero where it gives none).

```cpp
const std::vector<u8>& defaults() const;
```

### ComputeShader

A compute shader on the GPU, ready to dispatch (Renderer::dispatch). Loaded as an asset like Shader, from a
".comp.glsl" source: "&lt;path>" -> "&lt;path>.bowlsh", or the .glsl itself in dev builds. Resources follow SDL_GPU's
compute layout: set 0 = sampled textures and `readonly` storage, set 1 = read-write storage, set 2 = uniforms.

```cpp
class ComputeShader : NonCopyable
```

```cpp
~ComputeShader();
SDL_GPUComputePipeline* handle() const;
```

Threads per group (local_size_x/y/z), to work out how many groups cover a job.

```cpp
u32 threads(int axis) const;
u32 readonly_buffers() const;
u32 readwrite_buffers() const;
```

### GpuBuffer

Memory on the GPU that compute shaders read and write, and that sprites can be drawn from directly
(Renderer::submit with a GpuBuffer).

```cpp
class GpuBuffer : NonCopyable
```

```cpp
~GpuBuffer();
SDL_GPUBuffer* handle() const;
u32 size() const;
```

Replaces bytes from `offset` (outside a frame's drawing, like texture uploads).

```cpp
bool upload(const void* data, u32 bytes, u32 offset = 0);
```

### GpuDeviceDesc

```cpp
struct GpuDeviceDesc {
    bool debug = BOWL_DEV_TOOLS; // backend validation layers
    bool vsync = true;
    // Force a backend ("vulkan", "metal", "direct3d12"); empty = SDL's best choice for the platform.
    std::string driver;
    // Direct3D 12: where shaders compiled from HLSL at load time are kept, so later runs start faster. Empty: none.
    std::filesystem::path shader_cache;
};
```

### GpuDevice

```cpp
class GpuDevice : NonCopyable
```

```cpp
GpuDevice() = default;
~GpuDevice();
```

```cpp
bool init(SDL_Window* window, const GpuDeviceDesc& desc = {});
void shutdown();
```

```cpp
SDL_GPUDevice* handle() const;
SDL_Window* window() const;
SDL_GPUTextureFormat swapchain_format() const;
std::string_view driver() const;
ShaderBlobFormat shader_format() const;
void set_vsync(bool vsync);
bool vsync() const;
```

```cpp
std::unique_ptr<Texture> create_texture(const TextureDesc& desc);
```

Replaces all pixels of a texture (same size). Rebuilds mipmaps if it has them.

```cpp
bool update_texture(Texture& texture, const void* rgba8, bool premultiplied);
```

Replaces one rectangle. rgba8 points at the rectangle's top-left pixel in a source image `row_pixels` wide,
so a region of a larger CPU-side image can be sent without copying it out first.

```cpp
bool update_texture_region(Texture& texture, PixelRect rect, const void* rgba8, u32 row_pixels, bool premultiplied);
std::unique_ptr<Shader> create_shader(const ShaderBundle& bundle, std::string_view name);
std::unique_ptr<ComputeShader> create_compute_shader(const ShaderBundle& bundle, std::string_view name);
```

A zero-filled buffer of `bytes` for compute shaders and sprite drawing.

```cpp
std::unique_ptr<GpuBuffer> create_buffer(u32 bytes, std::string_view name);
```

Uploads raw bytes into a buffer through a temporary transfer buffer, on its own command buffer.

```cpp
bool upload(SDL_GPUBuffer* buffer, const void* data, u32 size, u32 offset = 0);
```

While a frame is being recorded, textures destroyed (a hot reload, an editor replacing a picture) may still be
drawn by it, so their GPU memory is let go only once the frame is submitted. The renderer holds and lets go.

```cpp
void hold_releases();
void let_go_releases();
```

Cached shader code on disk, by a hash of what it was made from. Used for D3D12's DXBC; works anywhere.

```cpp
std::optional<std::vector<u8>> load_cached_shader(const std::filesystem::path& dir, u64 key);
void store_cached_shader(const std::filesystem::path& dir, u64 key, std::span<const u8> code);
```

Registers Texture (PNG/JPG/TGA/BMP via stb_image), Shader, Font (.ttf/.otf), SpriteMaterial (.material),
SpriteSheet (Aseprite JSON) and Tilemap (Tiled .tmj) loaders with the asset manager.
default_filter: Nearest for pixel art, Linear (with mipmaps) for everything else.

```cpp
void register_asset_loaders(AssetManager& assets, GpuDevice& device, Filter default_filter = Filter::Linear);
```

## bowl/gfx/gpu_particles.hpp

`#include "bowl/gfx/gpu_particles.hpp"` · namespace `bowl::gfx`

### GpuParticleSettings

How a GpuParticles emitter behaves. Change it every frame if you like.

```cpp
struct GpuParticleSettings {
    f32 rate = 2000;          // new particles per second
    Vec2 position;            // the emitter, in the world
    f32 radius = 0;           // new particles start anywhere within this circle
    Vec2 velocity{0, -120};   // starting velocity
    f32 speed_spread = 80;    // plus up to this much speed in a random direction
    Vec2 gravity{0, 150};
    f32 drag = 0.5f;          // velocity lost per second (exponential)
    f32 life_min = 1, life_max = 2;
    f32 size_start = 8, size_end = 0;
    Color color_start = Color::white();
    Color color_end = Color::white().with_alpha(0);
    f32 depth = 0;            // every particle's sprite depth
};
```

### GpuParticles

Particles simulated and drawn entirely on the GPU: hundreds of thousands at the cost of one compute dispatch and one
draw a frame, with nothing sent from the CPU but the settings. Use the CPU ParticleEmitter for effects the game
reads back or that must be deterministic; use these for spectacle (sparks, snow, magic, fireworks).

```cpp
class GpuParticles : NonCopyable
```

```cpp
bool init(GpuDevice& device, AssetManager& assets, u32 capacity);
```

Queues this frame's step: spawns rate * dt new particles (plus any burst) and moves the rest.

```cpp
void update(Renderer& renderer, f32 dt, const GpuParticleSettings& settings);
```

Draws them with one texture (a soft dot, a spark). Call in the same frame, after update.

```cpp
void draw(Renderer& renderer, const Camera2D& camera, const Texture* texture,
          BlendMode blend = BlendMode::Additive) const;
```

Adds `count` particles at the next update, on top of the rate.

```cpp
void burst(u32 count);
```

Removes every particle at the next update.

```cpp
void clear();
u32 capacity() const;
```

## bowl/gfx/material.hpp

`#include "bowl/gfx/material.hpp"` · namespace `bowl::gfx`

How a material's sprites stand in the way of light_material lights.

```cpp
enum class Shadow : u8 {
    None,  // light passes through
    Shape, // the sprite's shape blocks light
    Faint, // blocks nothing and is never shadowed itself: the upright part of something whose footprint casts the shadow
};
```

### SpriteMaterial

A sprite's whole look, from a .material file (Bowl Editor's Materials workspace makes them):

```cpp
{
  "shader": "shaders/water.frag",                       // its own fragment shader; none: the sprite shader
  "params": {"speed": 1.5, "tint": [0.4, 0.8, 1, 1]},   // the shader's uniforms, by name
  "textures": {"u_noise": "textures/noise.png"},        // its extra textures, by name
  "blend": "alpha",                                     // alpha, additive, multiply or opaque
  "emissive": {"map": "props/lamp_emissive.png", "color": "#ffd890", "strength": 3},
  "normal": "props/lamp_normal.png",
  "shadow": "shape"                                     // none, shape or faint
}
```
Every key is optional. Parameters the file doesn't set take the defaults the shader's comments give (see
shaders.md). The emissive and normal maps line up with the sprite's own texture (the same layout, at any size), so
sprite sheet frames and atlas regions find their parts. The emissive map glows over the lit scene where it's bright
(black glows nowhere) in `color` times `strength`; with no map, a strength above 0 makes the sprite itself glow where
it's bright. Load it as an asset; it hot-reloads, and so do the textures and shader it names:

```cpp
Asset<gfx::SpriteMaterial> lamp = engine.assets().load<gfx::SpriteMaterial>("props/lamp.material");
world.create(ecs::Transform2D{{40, 80}}, gfx::Sprite{.texture = lamp_png, .material = lamp});
```

```cpp
class SpriteMaterial
```

```cpp
Asset<Shader> shader;
std::map<std::string, std::vector<f32>, std::less<>> params; // by uniform name; vectors and colors by component
std::map<std::string, Asset<Texture>, std::less<>> textures;  // the shader's extra textures, by its names for them
std::optional<BlendMode> blend;
Asset<Texture> emissive;
Color emissive_color = Color::white();
f32 emissive_strength = 0;
Asset<Texture> normal;
Shadow shadow = Shadow::None;
std::string preview; // the picture Bowl Editor shows it on; games ignore it
```

What its sprites draw with in the scene: null when it changes nothing there (the sprite shader, alpha blending).
Valid while the material lives.

```cpp
const Material* look() const;
```

What its glow draws with in the emissive layer (engine/shaders/emissive.frag); null when it doesn't glow.

```cpp
const Material* glow_look() const;
bool glows() const;
Color glow() const;
```

Sets a parameter (one number, or a vector's or color's components) for the next draw.

```cpp
void set_param(std::string_view name, std::span<const f32> value);
```

Reads a material from its JSON; the textures and shader it names load through `assets`. Problems (an unknown blend
mode, a parameter the shader doesn't have) go in `warnings` when given; the material is made anyway.

```cpp
std::unique_ptr<SpriteMaterial> parse_material(const json::Value& doc, AssetManager& assets,
                                               std::vector<std::string>* warnings = nullptr);
```

### SpriteLayers

The four layers a scene's sprites draw into, so their materials' glow, normal maps and shadows reach the renderer:

```cpp
layers.begin(engine.camera());               // a member: it reuses its memory
gfx::draw_maps(world, layers.scene, time);
gfx::draw_sprites(world, layers);
layers.submit(engine.renderer());
```
The glow needs nothing else; normal maps and shadows show under lights drawn with light_material.

```cpp
struct SpriteLayers
```

```cpp
SpriteBatch scene, emissive, normals, occluders;
```

```cpp
void begin(const Camera2D& camera, SpriteSort sort = SpriteSort::Depth, BlendMode blend = BlendMode::Alpha);
```

A sprite in its material's look (null: the plain sprite), with its glow, normal map and shadow.

```cpp
void draw(const Texture* texture, const SpriteInstance& sprite, const SpriteMaterial* material);
```

Submits each layer that has something in it.

```cpp
void submit(Renderer& renderer) const;
```

The name of a blend mode in material files ("alpha", "additive", "multiply", "opaque"), and back.

```cpp
std::string_view blend_name(BlendMode mode);
std::optional<BlendMode> blend_from_name(std::string_view name);
std::string_view shadow_name(Shadow shadow);
```

## bowl/gfx/particles.hpp

`#include "bowl/gfx/particles.hpp"` · namespace `bowl::gfx`

### ParticleSettings

What an emitter spawns. Reflected: tune it live in the inspector, save it with the world.

```cpp
struct ParticleSettings {
    f32 rate = 30;              // particles per second while emitting
    f32 lifetime_min = 0.6f, lifetime_max = 1.2f;
    f32 speed_min = 40, speed_max = 120;
    f32 direction = -pi / 2;    // radians; -pi/2 is up (y-down world)
    f32 spread = 0.6f;          // radians either side of direction
    Vec2 spawn_area;            // spawn within this box around the emitter (0 = a point)
    Vec2 gravity{0, 0};
    f32 drag = 0;               // fraction of velocity lost per second
    f32 size_start = 8, size_end = 2;
    Color color_start = Color::white();
    Color color_end = Color::white().with_alpha(0);
    f32 spin_min = 0, spin_max = 0; // radians per second
    u32 max_particles = 2000;
};
```

```cpp
BOWL_REFLECT(ParticleSettings, rate, lifetime_min, lifetime_max, speed_min, speed_max, direction, spread, spawn_area,
           gravity, drag, size_start, size_end, color_start, color_end, spin_min, spin_max, max_particles)
```

### ParticleSystem

The live particles of one emitter, stored as parallel arrays. Positions are in world space, so moving the emitter
leaves a trail.

```cpp
class ParticleSystem
```

```cpp
ParticleSystem();
explicit ParticleSystem(u64 seed);
```

Spawns `count` particles at `origin` right away (explosions, puffs).

```cpp
void burst(const ParticleSettings& s, Vec2 origin, u32 count);
```

Advances everything by dt, spawning at s.rate while `emitting`.

```cpp
void update(const ParticleSettings& s, Vec2 origin, f32 dt, bool emitting = true);
```

Draws each particle as `texture` (light_texture() makes soft round particles).

```cpp
void draw(SpriteBatch& batch, const Texture& texture, const ParticleSettings& s, f32 depth = 0) const;
```

```cpp
usize size() const;
void clear();
```

### ParticleEmitter

A particle emitter as a component, for effects that belong to an entity (a torch, a chimney, a spell). It emits
from the entity's GlobalTransform2D, or from `origin` if it has none. Settings save with the world; the live
particles don't.

```cpp
world.create(Transform2D{{40, 80}}, ParticleEmitter{.settings = smoke});
update_particle_emitters(world, dt);                    // in update() or a system
draw_particle_emitters(world, batch, renderer.light_texture());
```

```cpp
struct ParticleEmitter {
    ParticleSettings settings;
    bool emitting = true;
    Vec2 origin;
    f32 depth = 0;
    ParticleSystem system;
};
```

```cpp
BOWL_COMPONENT(ParticleEmitter, settings, emitting, origin, depth)
```

```cpp
void update_particle_emitters(ecs::World& world, f32 dt);
void draw_particle_emitters(ecs::World& world, SpriteBatch& batch, const Texture& texture);
```

## bowl/gfx/renderer.hpp

`#include "bowl/gfx/renderer.hpp"` · namespace `bowl::gfx`

### PostSettings

Whole-screen look. Defaults change nothing, so a game opts into each effect.

```cpp
struct PostSettings {
    // Color grading
    f32 exposure = 1;
    f32 contrast = 1;
    f32 saturation = 1;
    Color tint = Color::white();
    bool tonemap = false; // ACES-fitted; for scenes with lots of bright additive light
    // Bloom: pixels brighter than the threshold glow. Sprite colors may exceed 1 (HDR) to glow on purpose.
    bool bloom = false;
    f32 bloom_threshold = 0.9f;
    f32 bloom_knee = 0.4f;
    f32 bloom_intensity = 0.7f;
    f32 bloom_radius = 1.0f;
    // Vignette
    f32 vignette = 0; // 0..1 strength
    f32 vignette_radius = 0.55f;
    f32 vignette_softness = 0.6f;
    // 2D lighting: the scene is multiplied by (ambient + light map). Draw lights with submit_light.
    bool lighting = false;
    Color ambient{0.25f, 0.25f, 0.35f, 1};
    // A color-grading lookup table: an N*N x N strip of N slices (blue), each N x N (red across, green down), as
    // exported by most grading tools; make_neutral_lut writes the neutral one to start from. Applied after grading
    // and tonemapping. Keep the texture alive while it's set.
    const Texture* lut = nullptr;
    f32 lut_strength = 1;
    // Lens: colors split toward the edges (0.002-0.01 is subtle to strong), and moving film grain (0.02-0.1).
    f32 chromatic_aberration = 0;
    f32 grain = 0;
};
```

Your own full-screen pass. Its fragment shader gets the image so far at set 2 binding 0 (location 0 in: vec2 uv),
the material's textures at bindings 1-3 and its uniforms at set 3 binding 0. Scene effects work on the lit HDR scene
before bloom (heat haze, underwater wobble, a shockwave); Final effects on the graded image before the UI (pixelate,
CRT scanlines, a dither).

```cpp
enum class PostStage : u8 { Scene, Final };
```

### PostEffect

```cpp
struct PostEffect {
    const Material* material = nullptr; // kept by pointer: keep it alive while it's in the chain
    PostStage stage = PostStage::Scene;
    bool enabled = true;
};
```

### ComputeDispatch

One run of a compute shader (Renderer::dispatch).

```cpp
struct ComputeDispatch
```

```cpp
const ComputeShader* shader = nullptr;
std::vector<const GpuBuffer*> read; // `readonly` storage buffers, in binding order (set 0)
std::vector<GpuBuffer*> write;      // read-write storage buffers, in binding order (set 1)
std::vector<u8> uniforms;           // set 2 binding 0, std140
u32 groups[3] ={1, 1, 1};
```

```cpp
template <class T> void set_uniforms(const T& data);
```

Enough groups along x for one thread per item.

```cpp
void cover(u32 items);
```

### LightLook

How the built-in light shader (light_material) lights the scene.

```cpp
struct LightLook {
    // Lights shade normal-mapped sprites (submit_normals). Height is how far above the ground lights hang, relative to
    // their radius: low lights rake across bumps, high ones light them flat.
    bool normals = true;
    f32 height = 0.5f;
    // Occluders (submit_occluders) cast shadows from each light.
    bool shadows = true;
};
```

A material for light batches that adds normal mapping and shadows: begin the light batch with it.

```cpp
Material light_material(AssetManager& assets, const LightLook& look = {});
```

The neutral lookup table of `size` slices (16 or 32), as RGBA8 rows, to export, grade in an image editor and load
back as PostSettings::lut. Write it with write_png(path, size * size, size, pixels).

```cpp
std::vector<u8> make_neutral_lut(u32 size = 16);
```

### Renderer

Draws a frame:
  sprites -> HDR scene -> [normals, occluders -> lights -> light map] -> [lit scene + emissive] -> [scene effects]
  -> [bloom] -> final pass (grading, LUT, lens, vignette) -> [final effects] -> UI -> overlay
Everything submitted between begin_frame and end_frame draws in submission order; all sprite data for the frame
goes to the GPU in one upload.

```cpp
class Renderer : NonCopyable
```

#### Stats

```cpp
struct Stats {
    u32 sprites = 0;
    u32 draw_calls = 0;
    u32 batches = 0;
    u32 pipelines = 0;
};
```

#### Overlay

Hooks for overlays that bring their own GPU code (the dev UI): `prepare` runs before any pass (uploads),
`draw` inside the final pass, on top of everything.

```cpp
struct Overlay
```

```cpp
std::function<void(SDL_GPUCommandBuffer*)> prepare;
std::function<void(SDL_GPUCommandBuffer*, SDL_GPURenderPass*)> draw;
```

```cpp
Renderer() = default;
~Renderer();
```

```cpp
bool init(GpuDevice& device, AssetManager& assets);
void shutdown();
```

Acquires the next swapchain image. Returns false when there's nothing to draw into (minimized window);
skip drawing then, but still call end_frame.

```cpp
bool begin_frame();
void submit(const SpriteBatch& batch);
```

Lights: draw soft white-ish sprites (light_texture() is a good start) into the light map. Only used when
post().lighting is on. Blending is always additive.

```cpp
void submit_light(const SpriteBatch& batch);
```

UI: drawn after lighting and post-processing (unaffected by them), under the dev overlay.

```cpp
void submit_ui(const SpriteBatch& batch);
```

Emissive: sprites that give off their own light and ignore the light map (lamps, glowing eyes, screens). They
lie over the lit scene and still bloom.

```cpp
void submit_emissive(const SpriteBatch& batch);
```

Normal maps for lighting: draw the same sprites as in the scene, with their normal-map textures (OpenGL
convention, green up). Used by lights drawn with light_material.

```cpp
void submit_normals(const SpriteBatch& batch);
```

Shadow casters for light_material lights: anything drawn here (by its alpha) blocks light. Walls, trees, crates.

```cpp
void submit_occluders(const SpriteBatch& batch);
```

Runs a compute shader this frame, before anything is drawn. Dispatches run in the order they're made.

```cpp
void dispatch(ComputeDispatch job);
```

Sprites that a compute shader wrote into a buffer (SpriteInstance layout, `count` of them), all with one texture
(null: solid color). Drawn with the scene, in submission order.

```cpp
void submit(const GpuBuffer& sprites, u32 count, const Texture* texture, const Camera2D& camera,
            BlendMode blend = BlendMode::Alpha, const Material* material = nullptr);
```

Your own full-screen passes, run in order each frame.

```cpp
std::vector<PostEffect>& post_effects();
```

Sprites already in GPU memory, seen through `camera`. Nothing is uploaded; they draw in submission order with the
batches around them.

```cpp
void submit(const StaticSprites& sprites, const Camera2D& camera, BlendMode blend = BlendMode::Alpha,
            const Material* material = nullptr);
void end_frame();
```

```cpp
PostSettings& post();
```

The seconds that shaders' `bowl_time` holds (the App sets it to Engine::time() each frame).

```cpp
void set_time(f64 seconds);
f64 time() const;
void set_clear_color(Color c);
Color clear_color() const;
Vec2 target_size() const;
const Texture& white_texture() const;
```

A soft radial falloff, for lights and glows.

```cpp
const Texture& light_texture() const;
const Stats& stats() const; // totals of the last completed frame
void set_overlay(Overlay overlay);
```

Reads back the next rendered frame (as shown, overlays included) as RGBA8 rows, top to bottom. Stalls that frame.

```cpp
using CaptureCallback = std::function<void(u32 width, u32 height, std::span<const u8> rgba)>;
void capture_next_frame(CaptureCallback callback);
```

Encodes RGBA8 pixels as a PNG file.

```cpp
bool write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);
```

Decodes a PNG (or JPEG, BMP, TGA, or a BWTX picture bowl-cook made) in memory to RGBA8 pixels. False if it isn't a
picture.

```cpp
bool decode_png(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba);
```

## bowl/gfx/sprite.hpp

`#include "bowl/gfx/sprite.hpp"` · namespace `bowl::gfx`

### Sprite

An entity that looks like something: a texture (or a plain rectangle) drawn at the entity's GlobalTransform2D.
This is what the editor places and shows. Games are free to draw entities their own way instead.

```cpp
world.create(ecs::Transform2D{{120, 80}}, gfx::Sprite{.texture = assets.load<gfx::Texture>("props/barrel.png")});
gfx::draw_sprites(world, batch);   // after the transform systems have run
```

```cpp
struct Sprite {
    Asset<Texture> texture; // none: a solid rectangle in `color`
    Asset<SpriteMaterial> material; // its shader, glow, normal map and shadow (a .material file); none: plain
    Rect region;            // pixels of the texture to show; empty means all of it
    Vec2 size;              // world units before the transform's scale; zero means the region's pixel size
    Vec2 origin{0.5f, 0.5f};
    Color color = Color::white();
    f32 depth = 0;      // added to the draw depth
    bool y_sort = true; // draw depth is the entity's world y plus `depth`, so lower things draw in front
    bool visible = true;
};
```

```cpp
BOWL_COMPONENT(Sprite, texture, material, region, size, origin, color, depth, y_sort, visible)
```

The sprite's size in world units before scaling: `size`, or the region's or texture's pixel size, or 32 x 32 for a
rectangle with no size.

```cpp
Vec2 sprite_size(const Sprite& sprite);
```

Draws every visible Sprite that has a GlobalTransform2D, each with its material's shader. `skip` leaves entities out
(Bowl Editor's hidden ones).

```cpp
void draw_sprites(ecs::World& world, SpriteBatch& batch, const std::function<bool(ecs::Entity)>& skip = {});
```

The same, with materials' glow, normal maps and shadows in their layers too.

```cpp
void draw_sprites(ecs::World& world, SpriteLayers& layers, const std::function<bool(ecs::Entity)>& skip = {});
```

The axis-aligned world rectangle a sprite covers (enclosing it when rotated), for picking and culling. Empty if the
entity has no Sprite or no GlobalTransform2D.

```cpp
std::optional<Rect> sprite_bounds(const ecs::World& world, ecs::Entity e);
```

## bowl/gfx/sprite_sheet.hpp

`#include "bowl/gfx/sprite_sheet.hpp"` · namespace `bowl::gfx`

### SpriteFrame

```cpp
struct SpriteFrame {
    Rect source;        // pixels in the sheet texture (the trimmed piece)
    Vec2 offset;        // where the trimmed piece sits inside the full frame
    Vec2 size;          // the full (untrimmed) frame size
    f32 duration = 0.1f; // seconds
};
```

### FrameEvent

Something that happens on a frame of an animation: a footstep, the moment a swing hits.

```cpp
struct FrameEvent {
    u32 frame = 0; // counted from the animation's first frame
    std::string name;
};
```

### SpriteAnimation

```cpp
struct SpriteAnimation {
    enum class Direction : u8 { Forward, Reverse, PingPong, PingPongReverse };
    std::string name;
    u32 from = 0, to = 0; // inclusive frame range
    Direction direction = Direction::Forward;
    u32 repeat = 0;       // 0 = loop forever; otherwise play this many times, then hold the last frame
    std::vector<FrameEvent> events;
};
```

### SpriteSlice

A named rectangle on the sprite, from Aseprite's slices: hitboxes, the stretchable middle of a nine-slice panel, an
attachment point. It can change from frame to frame: each key applies from its frame on.

```cpp
struct SpriteSlice
```

#### Key

```cpp
struct Key {
    u32 frame = 0;
    Rect bounds;               // pixels in the full (untrimmed) frame
    std::optional<Rect> center; // nine-slice: the part that stretches, inside bounds
    std::optional<Vec2> pivot;  // inside bounds
};
```

```cpp
std::string name;
std::string data; // Aseprite's user data text
std::vector<Key> keys;
```

The key in effect on a frame, or null if the slice starts later.

```cpp
const Key* at(u32 frame) const;
```

### SpriteSheet

Frames + named animations over one texture. Load Aseprite "Export Sprite Sheet" JSON (hash or array layout) as an
asset, e.g. assets.load&lt;SpriteSheet>("characters/hero.json"); its PNG loads (and hot-reloads) alongside. Atlases made
by bowl-atlas load the same way.

```cpp
class SpriteSheet
```

```cpp
Asset<Texture> texture;
std::vector<SpriteFrame> frames;
std::vector<std::string> frame_names; // as exported ("hero 0.aseprite", "tree"); may be empty
std::vector<SpriteAnimation> animations;
std::vector<SpriteSlice> slices;
Vec2 pivot{0.5f, 0.5f}; // within the full frame; (0.5, 1) puts the feet on the position
```

```cpp
const SpriteAnimation* find(std::string_view name) const;
i32 index_of(std::string_view name) const;
```

A frame by its exported name, with or without the file extension; -1 if there's none.

```cpp
i32 frame_index(std::string_view name) const;
const SpriteSlice* slice(std::string_view name) const;
```

Parses Aseprite JSON. `image` receives meta.image (relative to the JSON file). Returns false on bad input.

Animation events come from a tag's user data: "3:step 7:step" fires "step" on the tag's 4th and 8th frames
(counted from 0). Separate them with spaces, commas or new lines; "=" works in place of ":".

```cpp
static bool parse_aseprite(const json::Value& doc, SpriteSheet& out, std::string& image, std::string& error);
```

A uniform grid, left to right then top to bottom, as one animation named "default".

```cpp
static SpriteSheet grid(Asset<Texture> texture, u32 cell_width, u32 cell_height, u32 count, f32 frame_seconds);
```

### SpriteAnimator

Plays animations from a sheet. A plain struct: use it as an ECS component and step it in a (parallel) system.

```cpp
struct SpriteAnimator
```

```cpp
Asset<SpriteSheet> sheet;
u32 animation = 0;
u32 frame = 0;        // absolute frame index into sheet->frames
f32 time = 0;         // seconds into the current frame
f32 speed = 1;
u32 loops_done = 0;
bool playing = true;
bool finished = false;    // a non-looping animation reached its end
bool frame_changed = false; // set for the update that changed frames
i8 step = 1;              // current direction while ping-ponging
u32 events_fired = 0;     // which of the animation's events the last update reached (bit per event)
```

Starts an animation by name (restart=false keeps going if it's already playing).

```cpp
bool play(std::string_view name, bool restart = false);
void update(f32 dt);
```

Whether the last update (or play) reached a frame with this event:
```cpp
if (anim.fired("step")) audio.play(footstep);
```

```cpp
bool fired(std::string_view event) const;
```

Draws one frame of a sheet with its pivot at `position`. scale may be negative to flip.

```cpp
void draw_frame(SpriteBatch& batch, const SpriteSheet& sheet, u32 frame, Vec2 position, Vec2 scale = {1, 1},
                Color color = Color::white(), f32 depth = 0, f32 rotation = 0);
```

Where a slice is in the world for a frame drawn at `position` and `scale` (as draw_frame draws it), for hitboxes
and attachment points. Empty if the sheet has no such slice on that frame.

```cpp
std::optional<Rect> slice_rect(const SpriteSheet& sheet, std::string_view slice, u32 frame, Vec2 position,
                               Vec2 scale = {1, 1});
```

Draws a frame as a nine-slice panel filling `dest`: the slice's center stretches, its borders don't. Without a
nine-slice center on that slice, draws nothing.

```cpp
void draw_nine_slice(SpriteBatch& batch, const SpriteSheet& sheet, std::string_view slice, u32 frame, Rect dest,
                     Color color = Color::white(), f32 depth = 0);
```

```cpp
void register_sprite_sheet_loader(AssetManager& assets);
```

## bowl/gfx/sprites.hpp

`#include "bowl/gfx/sprites.hpp"` · namespace `bowl::gfx`

### SpriteInstance

GPU layout of one sprite (std430). Must match engine/assets/shaders/sprite.vert.glsl.

```cpp
struct SpriteInstance {
    Vec2 position;
    Vec2 size{1, 1};
    Vec2 origin{0.5f, 0.5f}; // pivot within the sprite, 0..1
    f32 rotation = 0;         // radians
    f32 depth = 0;            // draw order: lower first (for isometric/top-down, use the y of the sprite's feet)
    Vec4 uv{0, 0, 1, 1};      // u0, v0, u1, v1
    Color color = Color::white();
};
```

### Camera2D

A 2D camera with y pointing down (screen convention), measured in logical points.

```cpp
struct Camera2D
```

```cpp
Vec2 position;            // world point at the center of the view
f32 zoom = 1;
f32 rotation = 0;
Vec2 viewport{1280, 720}; // logical size of the view; the App keeps this in sync with the window
bool pixel_snap = false;  // round the position to whole screen pixels (crisp pixel art)
```

```cpp
Mat4 view_projection() const;
Vec2 screen_to_world(Vec2 screen) const;
Vec2 world_to_screen(Vec2 world) const;
```

World-space rectangle the camera sees (axis-aligned bound when rotated).

```cpp
Rect visible_bounds() const;
```

A camera where world coordinates equal screen coordinates (UI).

```cpp
static Camera2D screen(Vec2 viewport);
```

```cpp
enum class SpriteSort : u8 {
    None,         // submission order (painter's algorithm, you sort)
    Texture,      // fewest draw calls; order between textures is arbitrary
    Depth,        // by depth, then texture for equal depth (the default)
};
```

How a batch's pixels combine with what's already drawn (colors are premultiplied).

```cpp
enum class BlendMode : u8 {
    Alpha,    // normal transparency
    Additive, // glows, fire, lights
    Multiply, // shadows, tinting
    Opaque,   // overwrite
};
```

### Material

A custom look for a batch, or for single sprites in one: your own fragment shader instead of the sprite shader. It
receives the sprite shader's outputs (location 0: vec2 uv, location 1: vec4 premultiplied color), the sprite texture
at set 2 binding 0, up to three extra textures at bindings 1-3, and `uniforms` (std140) at set 3 binding 0. Where
`uniforms` is shorter than the shader's block (or empty), the rest comes from the defaults the shader's comments
give, and members named `bowl_time` and `bowl_screen` are filled in by the renderer (see shaders.md).

```cpp
struct Material
```

```cpp
Asset<Shader> fragment;
std::array<const Texture*, 3> textures{}; // extra textures; null slots get a white texture
std::vector<u8> uniforms;                 // at most 256 bytes; use set_uniforms with a std140-shaped struct
std::optional<BlendMode> blend;           // instead of the batch's blend mode
```

```cpp
template <class T> void set_uniforms(const T& data);
```

### SpriteBatch

Collects sprites for one camera. Thousands of sprites cost one storage-buffer upload and one draw per texture
change. Fill it every frame and hand it to Renderer::submit.

```cpp
class SpriteBatch
```

```cpp
void begin(const Camera2D& camera, SpriteSort sort = SpriteSort::Depth, BlendMode blend = BlendMode::Alpha,
           const Material* material = nullptr);
void draw(const Texture& texture, const SpriteInstance& sprite);
```

Null texture: a solid quad in sprite.color.

```cpp
void draw(const Texture* texture, const SpriteInstance& sprite);
```

With a material of its own instead of the batch's (null: the batch's). Sprites with different materials still
sort together; each change of material costs a draw call. Keep the material alive until the frame is drawn.

```cpp
void draw(const Texture* texture, const SpriteInstance& sprite, const Material* material);
```

Convenience: a texture at its pixel size, centered at position.

```cpp
void draw(const Texture& texture, Vec2 position, Color color = Color::white(), f32 depth = 0);
```

A sub-rectangle of a texture (pixel coordinates), e.g. a sprite sheet frame.

```cpp
void draw_region(const Texture& texture, Rect source, Vec2 position, Vec2 size, Color color = Color::white(),
                 f32 depth = 0, f32 rotation = 0, Vec2 origin = {0.5f, 0.5f});
```

A filled rectangle (top-left corner at position).

```cpp
void draw_rect(Rect rect, Color color, f32 depth = 0);
```

A rectangle outline, `thickness` wide, inside `rect`.

```cpp
void draw_outline(Rect rect, Color color, f32 thickness = 1, f32 depth = 0);
```

Nine-slice: corners stay their size, edges stretch one way, the middle both. insets = left, top, right, bottom
border sizes in texture pixels (drawn at that many world units).

```cpp
void draw_nine_slice(const Texture& texture, Rect dest, Vec4 insets, Color color = Color::white(), f32 depth = 0);
```

The same, from a part of a texture (an atlas or sprite sheet frame) given in pixels.

```cpp
void draw_nine_slice(const Texture& texture, Rect source, Rect dest, Vec4 insets, Color color = Color::white(),
                     f32 depth = 0);
```

```cpp
usize size() const;
const Camera2D& camera() const;
```

### StaticSprites

Sprites uploaded to GPU memory once and then drawn every frame without being sent again: the ground of a big map,
a static backdrop. Build it from a filled batch (its sort order is kept); rebuild it when the sprites change.
Textures are kept by pointer, so rebuild after they hot-reload too. Draw it with Renderer::submit.

```cpp
class StaticSprites : NonCopyable
```

```cpp
StaticSprites() = default;
~StaticSprites();
```

```cpp
bool build(GpuDevice& device, const SpriteBatch& batch);
void clear();
usize size() const;
bool empty() const;
```

World rectangle around every sprite, for culling.

```cpp
Rect bounds() const;
```

The order a batch draws its sprites in (indices into it), per its SpriteSort.

```cpp
void sprite_order(const SpriteBatch& batch, std::vector<u32>& order);
```

## bowl/gfx/text.hpp

`#include "bowl/gfx/text.hpp"` · namespace `bowl::gfx`

### Font

A TrueType/OpenType font. Glyphs are rasterized on first use, at the exact pixel size requested, into shared atlas
pages, so text is sharp at any size without preprocessing. Loaded as an asset from .ttf/.otf files.

For text that changes size smoothly (zooming, pulsing titles), glyphs can also be drawn from distance fields: see
TextStyle::sdf.

```cpp
class Font : NonCopyable
```

#### Glyph

```cpp
struct Glyph {
    u16 page = 0;
    Rect source;    // pixels within the page
    Vec2 offset;    // from the pen position on the baseline to the bitmap's top-left, in pixels
    f32 advance = 0;
    bool visible = false; // false for whitespace
    bool color = false;   // a color glyph (emoji): drawn as it is, not in the text color
};
```

#### Metrics

```cpp
struct Metrics
```

```cpp
f32 ascent, descent, line_gap; // pixels at the given size; descent is negative
f32 line_height() const;
```

Distance-field glyphs are made once, at this pixel size, with this many pixels of falloff around the outline.

```cpp
static constexpr f32 sdf_size = 48;
static constexpr int sdf_spread = 6;
static constexpr u32 page_size = 1024;
```

device may be null for layout-only use (measuring, tests); pages then never reach the GPU.

```cpp
static std::unique_ptr<Font> from_memory(GpuDevice* device, std::vector<u8> ttf, std::string_view name);
~Font();
```

Glyph for a code point at a pixel size (rounded to whole pixels). A character this font lacks comes from the
first fallback font that has it, or is drawn as '?'.

```cpp
const Glyph& glyph(u32 codepoint, f32 pixel_size);
```

The distance-field glyph, in sdf_size pixels (scale it by size / sdf_size).

```cpp
const Glyph& sdf_glyph(u32 codepoint);
```

How far the pen moves after a character, without rasterizing anything.

```cpp
f32 advance(u32 codepoint, f32 pixel_size) const;
f32 kerning(u32 left, u32 right, f32 pixel_size) const;
Metrics metrics(f32 pixel_size) const;
```

Whether this font, or one of its fallbacks, has the character.

```cpp
bool has_glyph(u32 codepoint) const;
```

Fonts to look in, in order, for characters this one lacks: a CJK font, an emoji font, a symbols font.

```cpp
void set_fallbacks(std::vector<Asset<Font>> fonts);
const std::vector<Asset<Font>>& fallbacks() const;
```

The atlas pages as textures, uploaded if new glyphs were added since the last call.

```cpp
const Texture* page(u16 index);
const Texture* sdf_page(u16 index);
usize page_count() const;
```

A page's pixels (RGBA8, premultiplied, page_size x page_size), for tools and tests.

```cpp
std::span<const u8> page_pixels(u16 index) const;
```

Whether this build shapes text with HarfBuzz (BOWL_HARFBUZZ): joined Arabic letters, Indic and other complex scripts,
ligatures and OpenType kerning. Without it, characters are drawn one by one; right-to-left text is still put in
order (see bowl/gfx/bidi.hpp).

```cpp
bool text_shaping();
```

```cpp
enum class TextAlign : u8 { Left, Center, Right };
```

### TextStyle

```cpp
struct TextStyle {
    f32 size = 16;            // font size (ascent to descent) in world units; logical points for UI
    Color color = Color::white();
    TextAlign align = TextAlign::Left;
    // Which way paragraphs run: from their first letter (Auto), or as set. Right-to-left characters (Hebrew, Arabic) are
    // put in reading order either way.
    bidi::Direction direction = bidi::Direction::Auto;
    f32 max_width = 0;        // > 0: wrap words at this width
    f32 line_spacing = 1;     // multiplier on the font's line height
    // How many pixels one world unit covers, so glyphs rasterize at screen resolution. For a UI camera on a high-DPI
    // window use window.pixel_density(); for world text multiply by the camera zoom. Not needed with sdf.
    f32 raster_scale = 1;
    // Draw only the first N visible characters, with the layout of the full text (typewriter reveals). <0 = all.
    i32 max_glyphs = -1;
    // An outline around each letter, `outline` world units thick (up to a few), drawn under the text.
    f32 outline = 0;
    Color outline_color = Color::black();
    // A copy of the text under it, moved by `shadow` (0, 0: none).
    Vec2 shadow;
    Color shadow_color = Color::black().with_alpha(0.6f);
    // Read [color=#ffcc00]...[/color], [wave]...[/wave] and [shake]...[/shake] in the text ("[[" is a "["). Effects
    // move with `time` (seconds; pass engine.time()).
    bool markup = false;
    f32 time = 0;
    // Draw from distance fields: sharp at any size and rotation without re-rasterizing. The batch must be begun with
    // sdf_text_material(), which also draws SDF outlines (style.outline is ignored here).
    bool sdf = false;
};
```

### PlacedGlyph

One laid-out character, for drawing text your own way.

```cpp
struct PlacedGlyph {
    u32 codepoint = 0;        // the character drawn (a bracket in right-to-left text is drawn mirrored)
    u32 glyph_index = 0;      // shaped text: the glyph in its font (a character can be several glyphs, or share one)
    u32 index = 0;            // among the visible characters, from 0, in reading order
    u16 page = 0;
    bool sdf = false;
    bool color_glyph = false; // an emoji: its color is white with the text's alpha, so it keeps its own colors
    Rect source;              // in the font page
    Vec2 position;            // top-left corner in the world
    Vec2 size;                // in the world
    Color color;
};
```

Draws UTF-8 text with its top-left (or top-center/right, per align) at position. '\n' breaks lines.
Returns the size of the laid-out block.

```cpp
Vec2 draw_text(SpriteBatch& batch, Font& font, std::string_view text, Vec2 position, const TextStyle& style,
               f32 depth = 0);
Vec2 measure_text(Font& font, std::string_view text, const TextStyle& style);
```

The characters draw_text would draw (shadows and outlines aside), in order.

```cpp
std::vector<PlacedGlyph> layout_text(Font& font, std::string_view text, Vec2 position, const TextStyle& style);
```

The text with markup tags taken out.

```cpp
std::string strip_markup(std::string_view text);
```

### SdfTextLook

How distance-field text looks: the outline is in parts of the glyph's falloff (0 to about 0.4), softness blurs the
edge (0 is crisp).

```cpp
struct SdfTextLook {
    Color outline_color = Color::black();
    f32 outline = 0;
    f32 softness = 0;
};
```

A material for batches of SDF text. Keep it alive while the batch is used.

```cpp
Material sdf_text_material(AssetManager& assets, const SdfTextLook& look = {});
```

## bowl/gfx/tilemap.hpp

`#include "bowl/gfx/tilemap.hpp"` · namespace `bowl::gfx`

Tiled maps (mapeditor.org), loaded from Tiled's JSON (.tmj, .tsj) or XML (.tmx, .tsx) files. Orthogonal, isometric
(diamond), staggered and hexagonal orientations; finite and infinite maps; CSV, XML and base64 (+zlib/gzip) data.

```cpp
inline constexpr u32 tile_flip_h = 0x80000000u;
inline constexpr u32 tile_flip_v = 0x40000000u;
inline constexpr u32 tile_flip_d = 0x20000000u; // anti-diagonal (transpose)
inline constexpr u32 tile_gid_mask = 0x0FFFFFFFu;
```

The most cells a tile layer can have (4096 x 4096, 64 MB). A map with a bigger one fails to load, so a broken or
hostile map file can't make a game ask for gigabytes.

```cpp
inline constexpr u64 max_layer_cells = u64{1};
<< 24;
```

### TileFrame

```cpp
struct TileFrame {
    u32 tile; // local id within the tileset
    f32 seconds;
};
```

### TileInfo

```cpp
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
```

### WangSet

A Tiled terrain set ("Wang set"): which tile fits which terrains at its corners and edges. Kept as data, for drawing
terrain from code (procedural maps, digging, farm plots): see corner_tile and fill_from_corners.

```cpp
struct WangSet
```

```cpp
enum class Type : u8 { Corner, Edge, Mixed };
```

#### Terrain

```cpp
struct Terrain {
    std::string name;
    Color color;
    f32 probability = 1;
    json::Value properties;
};
```

#### Tile

```cpp
struct Tile {
    u32 tile; // local id in the tileset
    // Terrain at each spot, 1-based into `terrains` (0: none), going round from the top:
    // top, top-right, right, bottom-right, bottom, bottom-left, left, top-left. Corners are 1, 3, 5, 7.
    std::array<u8, 8> wang{};
};
```

```cpp
std::string name;
Type type = Type::Corner;
std::vector<Terrain> terrains;
std::vector<Tile> tiles;
json::Value properties;
```

Terrain number by name (1-based), or 0.

```cpp
u8 terrain(std::string_view terrain_name) const;
```

Local ids of tiles whose terrains match `want` (a 0 in `want` matches anything).

```cpp
std::vector<u32> matching(const std::array<u8, 8>& want) const;
```

A tile with these terrains at its corners (top-left, top-right, bottom-right, bottom-left), choosing among
variants by `pick` (any number: a hash of the cell keeps choices stable). Nothing if no tile fits.

```cpp
std::optional<u32> corner_tile(u8 top_left, u8 top_right, u8 bottom_right, u8 bottom_left, u32 pick = 0) const;
```

### Tileset

```cpp
struct Tileset
```

```cpp
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
```

```cpp
const TileInfo* tile(u32 local_id) const;
Rect source_rect(u32 local_id) const; // pixels in `image`
const WangSet* wang_set(std::string_view set_name) const;
```

### MapObject

```cpp
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
```

### MapLayer

```cpp
struct MapLayer
```

```cpp
enum class Kind : u8 { Tiles, Objects, Image };
Kind kind = Kind::Tiles;
std::string name; // group layers flatten to "group/child"
bool visible = true;
f32 opacity = 1;
Color tint = Color::white();
Vec2 offset;
json::Value properties;
```

Tiles: a rectangle of cells (infinite maps are merged into one rectangle starting at origin_x/y).

```cpp
i32 origin_x = 0, origin_y = 0;
u32 width = 0, height = 0;
std::vector<u32> cells; // gids with flip flags, row-major; 0 = empty
```

Objects

```cpp
std::vector<MapObject> objects;
```

Image

```cpp
std::string image_path;
Asset<Texture> image;
```

Gid at a tile coordinate (0 outside the layer).

```cpp
u32 at(i32 x, i32 y) const;
```

### Tilemap

```cpp
class Tilemap
```

```cpp
enum class Orientation : u8 { Orthogonal, Isometric, Staggered, Hexagonal };
```

```cpp
Orientation orientation = Orientation::Orthogonal;
```

Staggered and hexagonal maps: which axis is shifted every other row or column, whether the shifted ones are the
even or the odd ones, and (hexagonal) the length of the hexagon's flat side in pixels.

```cpp
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
```

Reads a .tmj document. `read_file` resolves paths relative to `base_dir` (external .tsj tilesets).
Textures aren't loaded here; the asset loader fills them in.

```cpp
static bool parse(const json::Value& doc, std::string_view base_dir,
                  const std::function<std::optional<std::string>(const std::string& path)>& read_file,
                  Tilemap& out, std::string& error);
```

Reads a .tmx document (XML). External tilesets may be .tsx or .tsj.

```cpp
static bool parse_tmx(std::string_view text, std::string_view base_dir,
                      const std::function<std::optional<std::string>(const std::string& path)>& read_file,
                      Tilemap& out, std::string& error);
```

The map at `path` (a .tmj or .tmx, by its extension) as one .tmj document that needs no other file, for shipping:
external tilesets and object templates put in place (their image paths moved to read from the map's folder), and
layer data as plain arrays of gids. bowl-cook stores it as binary JSON, which parse() and the loader read. The
tileset and template files it read are added to `files_read`.

```cpp
static std::optional<json::Value> flatten(std::string_view text, std::string_view path,
                                          const std::function<std::optional<std::string>(const std::string& path)>& read_file,
                                          std::vector<std::string>& files_read, std::string& error);
```

```cpp
const MapLayer* layer(std::string_view name) const;
```

The tileset owning a gid (flip flags ignored), or null.

```cpp
const Tileset* tileset_for(u32 gid) const;
const TileInfo* tile_info(u32 gid) const;
```

Custom property of the tile at a cell, e.g. map.tile_property("ground", x, y, "solid").as_bool()

```cpp
const json::Value& tile_property(std::string_view layer_name, i32 x, i32 y, std::string_view key) const;
```

Map pixel coordinates of a cell's anchor, where Tiled puts tile (x, y): its top-left (orthogonal, and the top-left
of the tile's box for staggered and hexagonal maps) or the top corner of the diamond (isometric).

```cpp
Vec2 cell_to_world(f32 x, f32 y) const;
```

Inverse: fractional cell coordinates of a map point; floor() them for the cell. Staggered and hexagonal maps
return the whole cell under the point.

```cpp
Vec2 world_to_cell(Vec2 world) const;
```

The y a tile in cell (x, y) sorts by when y-sorted: the middle of the cell, where the feet of a sprite standing
in the middle of it would be. Sprites sort by their feet, so they go behind a tile from the far sides and in
front from the near ones.

```cpp
f32 sort_y(i32 x, i32 y) const;
```

The cell under a map point.

```cpp
std::pair<i32, i32> cell_at(Vec2 world) const;
```

Size of the whole map in pixels (finite maps).

```cpp
Vec2 pixel_size() const;
```

### TilemapDrawOptions

```cpp
struct TilemapDrawOptions {
    Vec2 position;        // where the map's origin goes in the world
    f32 time = 0;         // seconds, for animated tiles
    f32 depth = 0;        // base depth; each layer adds layer_depth_step
    f32 layer_depth_step = 1;
    // Depth = the middle of the tile's cell (Tilemap::sort_y, + base), so sprites at depth = their feet's y go behind
    // tall tiles from the far sides and in front from the near ones (isometric).
    // Which layers sort: those with a Tiled bool property "ysort"; if no layer has it, every layer but the first.
    // Layers that don't sort draw underneath everything sorted (ground), except those with the bool property "above",
    // which draw over it (roofs, treetops, arches: what characters walk under).
    bool y_sort = false;
    std::string_view only_layer; // draw just this layer
    // Which layers to draw: all, only the flat ones (ground) or only those drawn with the sprites (y-sorted, and
    // "above"). TilemapCache draws the flat ones; pass Sorted to draw the rest each frame.
    enum class Layers : u8 { All, Flat, Sorted };
    Layers layers = Layers::All;
};
```

Draws the visible part of the map (culled against the batch camera's view).

```cpp
void draw_tilemap(SpriteBatch& batch, const Tilemap& map, const TilemapDrawOptions& options = {});
```

Fills a tile layer from terrain at the corners of its cells: `corners` is (width + 1) x (height + 1) terrain numbers
of `set` (row by row), and every cell gets a tile that fits its four corners, chosen among variants by position so
the same corners always give the same map. The layer becomes width x height at its origin. Returns how many cells
had no fitting tile (left empty).

```cpp
usize fill_from_corners(MapLayer& layer, const Tileset& tileset, const WangSet& set, std::span<const u8> corners,
                        u32 width, u32 height);
```

### TilemapCache

Draws large maps cheaply: the flat layers are cut into chunks of chunk_cells x chunk_cells tiles, each built once into
StaticSprites in GPU memory, so a frame only submits the chunks on screen. Animated tiles are still drawn every
frame. Y-sorted and "above" layers aren't cached: draw them with draw_tilemap and `.layers = Layers::Sorted`.

Chunks rebuild by themselves when the map asset or its textures reload; call invalidate() after changing cells.

```cpp
class TilemapCache : NonCopyable
```

```cpp
static constexpr i32 chunk_cells = 32;
```

Submits the visible part of the map's flat layers to the renderer, as seen through `camera`, in layer order.

```cpp
void submit(Renderer& renderer, GpuDevice& device, const Camera2D& camera, const Tilemap& map,
            const TilemapDrawOptions& options = {});
void invalidate();
usize chunk_count() const;
```

```cpp
void register_tilemap_loader(AssetManager& assets);
```

"maps/../tiles/a.png" -> "tiles/a.png"

```cpp
std::string normalize_path(std::string_view path);
```

### Map

A Tiled map placed in the world, at its entity's position (GlobalTransform2D; maps don't turn or scale). It's what
Bowl Editor puts in a level for a .tmj or .tmx file, and draw_maps draws every one.

```cpp
struct Map {
    Asset<Tilemap> map;
    f32 depth = 0;      // added to every tile's draw depth
    bool y_sort = true; // tall layers sort with sprites by the middles of their cells (TilemapDrawOptions::y_sort);
    // off: all of it draws underneath sprites
    bool visible = true;
};
```

```cpp
BOWL_COMPONENT(Map, map, depth, y_sort, visible)
```

Draws every visible Map that has a GlobalTransform2D, into the same batch as the sprites so they sort together.
`time` animates tiles; `skip` leaves entities out (Bowl Editor's hidden ones).

```cpp
void draw_maps(ecs::World& world, SpriteBatch& batch, f32 time = 0,
               const std::function<bool(ecs::Entity)>& skip = {});
```

