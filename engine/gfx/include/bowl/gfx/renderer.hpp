#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/gfx/device.hpp"
#include "bowl/gfx/sprites.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace bowl::gfx {

// Whole-screen look. Defaults change nothing, so a game opts into each effect.
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

// Your own full-screen pass. Its fragment shader gets the image so far at set 2 binding 0 (location 0 in: vec2 uv),
// the material's textures at bindings 1-3 and its uniforms at set 3 binding 0. Scene effects work on the lit HDR scene
// before bloom (heat haze, underwater wobble, a shockwave); Final effects on the graded image before the UI (pixelate,
// CRT scanlines, a dither).
enum class PostStage : u8 { Scene, Final };
struct PostEffect {
    const Material* material = nullptr; // kept by pointer: keep it alive while it's in the chain
    PostStage stage = PostStage::Scene;
    bool enabled = true;
};

// One run of a compute shader (Renderer::dispatch).
struct ComputeDispatch {
    const ComputeShader* shader = nullptr;
    std::vector<const GpuBuffer*> read; // `readonly` storage buffers, in binding order (set 0)
    std::vector<GpuBuffer*> write;      // read-write storage buffers, in binding order (set 1)
    std::vector<u8> uniforms;           // set 2 binding 0, std140
    u32 groups[3] = {1, 1, 1};

    template <class T> void set_uniforms(const T& data) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 16 == 0, "uniform blocks are std140");
        uniforms.assign(reinterpret_cast<const u8*>(&data), reinterpret_cast<const u8*>(&data) + sizeof(T));
    }
    // Enough groups along x for one thread per item.
    void cover(u32 items) { groups[0] = shader ? (items + shader->threads(0) - 1) / shader->threads(0) : 0; }
};

// How the built-in light shader (light_material) lights the scene.
struct LightLook {
    // Lights shade normal-mapped sprites (submit_normals). Height is how far above the ground lights hang, relative to
    // their radius: low lights rake across bumps, high ones light them flat.
    bool normals = true;
    f32 height = 0.5f;
    // Occluders (submit_occluders) cast shadows from each light.
    bool shadows = true;
};
// A material for light batches that adds normal mapping and shadows: begin the light batch with it.
Material light_material(AssetManager& assets, const LightLook& look = {});

// The neutral lookup table of `size` slices (16 or 32), as RGBA8 rows, to export, grade in an image editor and load
// back as PostSettings::lut. Write it with write_png(path, size * size, size, pixels).
std::vector<u8> make_neutral_lut(u32 size = 16);

// Draws a frame:
//   sprites -> HDR scene -> [normals, occluders -> lights -> light map] -> [lit scene + emissive] -> [scene effects]
//   -> [bloom] -> final pass (grading, LUT, lens, vignette) -> [final effects] -> UI -> overlay
// Everything submitted between begin_frame and end_frame draws in submission order; all sprite data for the frame
// goes to the GPU in one upload.
class Renderer : NonCopyable {
public:
    struct Stats {
        u32 sprites = 0;
        u32 draw_calls = 0;
        u32 batches = 0;
        u32 pipelines = 0;
    };
    // Hooks for overlays that bring their own GPU code (the dev UI): `prepare` runs before any pass (uploads),
    // `draw` inside the final pass, on top of everything.
    struct Overlay {
        std::function<void(SDL_GPUCommandBuffer*)> prepare;
        std::function<void(SDL_GPUCommandBuffer*, SDL_GPURenderPass*)> draw;
    };

    Renderer() = default;
    ~Renderer();

    bool init(GpuDevice& device, AssetManager& assets);
    void shutdown();

    // Acquires the next swapchain image. Returns false when there's nothing to draw into (minimized window);
    // skip drawing then, but still call end_frame.
    bool begin_frame();
    void submit(const SpriteBatch& batch);
    // Lights: draw soft white-ish sprites (light_texture() is a good start) into the light map. Only used when
    // post().lighting is on. Blending is always additive.
    void submit_light(const SpriteBatch& batch);
    // UI: drawn after lighting and post-processing (unaffected by them), under the dev overlay.
    void submit_ui(const SpriteBatch& batch);
    // Emissive: sprites that give off their own light and ignore the light map (lamps, glowing eyes, screens). They
    // lie over the lit scene and still bloom.
    void submit_emissive(const SpriteBatch& batch);
    // Normal maps for lighting: draw the same sprites as in the scene, with their normal-map textures (OpenGL
    // convention, green up). Used by lights drawn with light_material.
    void submit_normals(const SpriteBatch& batch);
    // Shadow casters for light_material lights: anything drawn here (by its alpha) blocks light. Walls, trees, crates.
    void submit_occluders(const SpriteBatch& batch);
    // Runs a compute shader this frame, before anything is drawn. Dispatches run in the order they're made.
    void dispatch(ComputeDispatch job);
    // Sprites that a compute shader wrote into a buffer (SpriteInstance layout, `count` of them), all with one texture
    // (null: solid color). Drawn with the scene, in submission order.
    void submit(const GpuBuffer& sprites, u32 count, const Texture* texture, const Camera2D& camera,
                BlendMode blend = BlendMode::Alpha, const Material* material = nullptr);
    // Your own full-screen passes, run in order each frame.
    std::vector<PostEffect>& post_effects() { return post_effects_; }
    // Sprites already in GPU memory, seen through `camera`. Nothing is uploaded; they draw in submission order with the
    // batches around them.
    void submit(const StaticSprites& sprites, const Camera2D& camera, BlendMode blend = BlendMode::Alpha,
                const Material* material = nullptr);
    void end_frame();

    PostSettings& post() { return post_; }
    void set_clear_color(Color c) { clear_color_ = c; }
    Color clear_color() const { return clear_color_; }
    Vec2 target_size() const { return target_size_; }
    const Texture& white_texture() const { return *white_; }
    // A soft radial falloff, for lights and glows.
    const Texture& light_texture() const { return *light_; }
    const Stats& stats() const { return last_stats_; } // totals of the last completed frame
    void set_overlay(Overlay overlay) { overlay_ = std::move(overlay); }

    // Reads back the next rendered frame (as shown, overlays included) as RGBA8 rows, top to bottom. Stalls that frame.
    using CaptureCallback = std::function<void(u32 width, u32 height, std::span<const u8> rgba)>;
    void capture_next_frame(CaptureCallback callback) { capture_ = std::move(callback); }

private:
    // A run of sprites with one texture. The texture is kept as its GPU handle, read when the run is recorded:
    // something drawn this frame may be destroyed before the frame ends (the device holds its memory until then).
    struct Draw {
        u32 first; // sprite index in the frame buffer
        u32 count;
        SDL_GPUTexture* texture;
        Filter filter;
    };
    static Draw draw_of(u32 first, u32 count, const Texture& t) { return {first, count, t.handle(), t.filter()}; }
    struct Pass {
        Mat4 view_projection;
        u32 first_draw, draw_count;
        BlendMode blend;
        const Material* material;
        SDL_GPUBuffer* buffer = nullptr; // null: this frame's sprites
    };
    struct PipelineKey {
        u32 vertex, fragment;
        SDL_GPUTextureFormat format;
        BlendMode blend;
        bool operator==(const PipelineKey&) const = default;
    };
    struct PipelineKeyHash {
        usize operator()(const PipelineKey& k) const {
            return (static_cast<usize>(k.vertex) * 73856093u) ^ (static_cast<usize>(k.fragment) * 19349663u) ^
                   (static_cast<usize>(k.format) * 83492791u) ^ static_cast<usize>(k.blend);
        }
    };
    struct CachedPipeline {
        SDL_GPUGraphicsPipeline* pipeline;
        u64 last_used;
    };

    void record(const SpriteBatch& batch, std::vector<Pass>& passes);
    SDL_GPUGraphicsPipeline* pipeline(const Shader& vs, const Shader& fs, SDL_GPUTextureFormat format, BlendMode blend);
    bool ensure_targets(u32 width, u32 height);
    // `extras`: what a material's empty texture slots (bindings 1-3) get.
    void draw_sprites(SDL_GPURenderPass* pass, const std::vector<Pass>& passes, SDL_GPUTextureFormat format,
                      std::array<SDL_GPUTexture*, 3> extras = {});
    // Sprites into a target of its own, cleared first.
    void draw_into(SDL_GPUTexture* target, Color clear, const std::vector<Pass>& passes, SDL_GPUTextureFormat format,
                   std::array<SDL_GPUTexture*, 3> extras = {});
    void fullscreen(SDL_GPUTexture* target, bool clear, const Shader& fragment, SDL_GPUTextureFormat format,
                    BlendMode blend, std::span<const SDL_GPUTexture* const> inputs, const void* uniforms, u32 size,
                    SDL_GPURenderPass** keep_open = nullptr);
    void run_bloom(SDL_GPUTexture* input);
    void release_targets();
    // A full-size target made the first time it's needed (most games never need some of them).
    SDL_GPUTexture* lazy_target(SDL_GPUTexture*& target, SDL_GPUTextureFormat format, const char* name);

    GpuDevice* device_ = nullptr;
    // Engine shaders (hot-reloadable).
    Asset<Shader> sprite_vs_, sprite_fs_, fullscreen_vs_, final_fs_, bloom_pre_fs_, bloom_down_fs_, bloom_up_fs_,
        composite_fs_;
    std::unordered_map<PipelineKey, CachedPipeline, PipelineKeyHash> pipelines_;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    std::unique_ptr<Texture> white_, black_, clear_, flat_normal_, light_;

    SDL_GPUBuffer* sprite_buffer_ = nullptr;
    SDL_GPUTransferBuffer* sprite_transfer_ = nullptr;
    u32 sprite_capacity_ = 0;

    // Render targets, recreated when the window size changes.
    static constexpr SDL_GPUTextureFormat hdr_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    static constexpr int bloom_levels = 6;
    SDL_GPUTexture* scene_ = nullptr;  // HDR
    SDL_GPUTexture* final_ = nullptr;  // swapchain format: what the window shows
    SDL_GPUTexture* lightmap_ = nullptr; // HDR, half resolution
    // Made when first used: the second scene buffer (lighting, scene effects), emissive sprites, normals, occluders,
    // and the second final buffer (final effects).
    SDL_GPUTexture* scene2_ = nullptr;
    SDL_GPUTexture* emissive_ = nullptr;
    SDL_GPUTexture* normals_ = nullptr;
    SDL_GPUTexture* occluders_ = nullptr;
    SDL_GPUTexture* final2_ = nullptr;
    SDL_GPUTexture* bloom_[bloom_levels] = {};
    u32 bloom_w_[bloom_levels] = {}, bloom_h_[bloom_levels] = {};
    u32 target_w_ = 0, target_h_ = 0;

    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPUTexture* swapchain_ = nullptr;
    Vec2 target_size_;
    Color clear_color_ = Color::hex(0x101018);
    PostSettings post_;
    u64 frame_ = 0;

    std::vector<SpriteInstance> frame_sprites_;
    std::vector<Draw> draws_;
    std::vector<Pass> passes_, light_passes_, ui_passes_, emissive_passes_, normal_passes_, occluder_passes_;
    std::vector<PostEffect> post_effects_;
    std::vector<ComputeDispatch> compute_;
    std::vector<u32> order_;
    Stats stats_, last_stats_;
    Overlay overlay_;
    CaptureCallback capture_;
};

// Encodes RGBA8 pixels as a PNG file.
bool write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);
// Decodes a PNG (or JPEG, BMP, TGA, or a BWTX picture bowl-cook made) in memory to RGBA8 pixels. False if it isn't a
// picture.
bool decode_png(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba);

} // namespace bowl::gfx
