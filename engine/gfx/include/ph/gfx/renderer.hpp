#pragma once

#include "ph/core/assets.hpp"
#include "ph/gfx/device.hpp"
#include "ph/gfx/sprites.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace ph::gfx {

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
};

// Draws a frame:
//   sprites -> HDR scene -> [lights -> light map] -> [bloom] -> final pass (grading, vignette) -> UI -> overlay
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
    struct Draw {
        u32 first; // sprite index in the frame buffer
        u32 count;
        const Texture* texture;
    };
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
    void draw_sprites(SDL_GPURenderPass* pass, const std::vector<Pass>& passes, SDL_GPUTextureFormat format);
    void fullscreen(SDL_GPUTexture* target, bool clear, const Shader& fragment, SDL_GPUTextureFormat format,
                    BlendMode blend, std::span<const SDL_GPUTexture* const> inputs, const void* uniforms, u32 size,
                    SDL_GPURenderPass** keep_open = nullptr);
    void run_bloom();
    void release_targets();

    GpuDevice* device_ = nullptr;
    // Engine shaders (hot-reloadable).
    Asset<Shader> sprite_vs_, sprite_fs_, fullscreen_vs_, final_fs_, bloom_pre_fs_, bloom_down_fs_, bloom_up_fs_;
    std::unordered_map<PipelineKey, CachedPipeline, PipelineKeyHash> pipelines_;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    std::unique_ptr<Texture> white_, black_, light_;

    SDL_GPUBuffer* sprite_buffer_ = nullptr;
    SDL_GPUTransferBuffer* sprite_transfer_ = nullptr;
    u32 sprite_capacity_ = 0;

    // Render targets, recreated when the window size changes.
    static constexpr SDL_GPUTextureFormat hdr_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    static constexpr int bloom_levels = 6;
    SDL_GPUTexture* scene_ = nullptr;  // HDR
    SDL_GPUTexture* final_ = nullptr;  // swapchain format: what the window shows
    SDL_GPUTexture* lightmap_ = nullptr; // HDR, half resolution
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
    std::vector<Pass> passes_, light_passes_, ui_passes_;
    std::vector<u32> order_;
    Stats stats_, last_stats_;
    Overlay overlay_;
    CaptureCallback capture_;
};

// Encodes RGBA8 pixels as a PNG file.
bool write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);

} // namespace ph::gfx
