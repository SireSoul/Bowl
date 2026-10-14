#pragma once

#include "ph/core/assets.hpp"
#include "ph/gfx/device.hpp"
#include "ph/gfx/sprites.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ph::gfx {

// Draws a frame: everything submitted between begin_frame and end_frame, in submission order, in one render pass.
// All sprite data for the frame goes to the GPU in a single upload.
class Renderer : NonCopyable {
public:
    struct Stats {
        u32 sprites = 0;
        u32 draw_calls = 0;
        u32 batches = 0;
    };
    // Hooks for overlays that bring their own GPU code (the dev UI): `prepare` runs before the render pass (uploads),
    // `draw` inside it, after everything else.
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
    void end_frame();

    void set_clear_color(Color c) { clear_color_ = c; }
    Color clear_color() const { return clear_color_; }
    // Size of the current target in pixels.
    Vec2 target_size() const { return target_size_; }
    const Texture& white_texture() const { return *white_; }
    const Stats& stats() const { return stats_; }
    void set_overlay(Overlay overlay) { overlay_ = std::move(overlay); }

    // Reads back the next rendered frame (without overlays) as RGBA8 rows, top to bottom. Stalls that one frame.
    using CaptureCallback = std::function<void(u32 width, u32 height, std::span<const u8> rgba)>;
    void capture_next_frame(CaptureCallback callback) { capture_ = std::move(callback); }

private:
    bool rebuild_pipeline();

    struct Draw {
        u32 first;   // sprite index in the frame buffer
        u32 count;
        const Texture* texture;
    };
    struct Pass {
        Mat4 view_projection;
        u32 first_draw, draw_count;
    };

    GpuDevice* device_ = nullptr;
    Asset<Shader> vertex_, fragment_;
    u32 vertex_version_ = 0, fragment_version_ = 0;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    std::unique_ptr<Texture> white_;

    SDL_GPUBuffer* sprite_buffer_ = nullptr;
    SDL_GPUTransferBuffer* sprite_transfer_ = nullptr;
    u32 sprite_capacity_ = 0;

    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPUTexture* target_ = nullptr;
    SDL_GPUTexture* scene_ = nullptr;
    u32 scene_w_ = 0, scene_h_ = 0;
    CaptureCallback capture_;
    Vec2 target_size_;
    Color clear_color_ = Color::hex(0x101018);

    std::vector<SpriteInstance> frame_sprites_;
    std::vector<Draw> draws_;
    std::vector<Pass> passes_;
    std::vector<u32> order_;
    Stats stats_;
    Overlay overlay_;
};

// Encodes RGBA8 pixels as a PNG file.
bool write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);

} // namespace ph::gfx
