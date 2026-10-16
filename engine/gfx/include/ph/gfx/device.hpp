#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/math.hpp"
#include "ph/core/shader_bundle.hpp"
#include "ph/core/types.hpp"

#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace ph::gfx {

class GpuDevice;

enum class Filter : u8 { Nearest, Linear };

// A rectangle of whole pixels in a texture, top-left origin.
struct PixelRect {
    u32 x = 0, y = 0, w = 0, h = 0;
};

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    const void* rgba8 = nullptr; // width*height*4 bytes, straight alpha unless premultiplied is set
    bool premultiplied = false;
    Filter filter = Filter::Linear;
    bool mipmaps = true; // only with Linear filtering
    std::string_view name;
};

class Texture : NonCopyable {
public:
    ~Texture();
    SDL_GPUTexture* handle() const { return texture_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    Vec2 size() const { return {static_cast<f32>(width_), static_cast<f32>(height_)}; }
    Filter filter() const { return filter_; }
    // Unique per texture object (never reused), for sorting draws.
    u32 id() const { return id_; }

private:
    friend class GpuDevice;
    Texture() = default;
    GpuDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    u32 width_ = 0, height_ = 0, id_ = 0;
    Filter filter_ = Filter::Linear;
    bool mipmapped_ = false;
};

// One compiled shader stage on the GPU. Loaded as an asset from "<path>" -> "<path>.phsh" (or, in dev builds,
// compiled live from "<path>.glsl" so edits hot-reload).
class Shader : NonCopyable {
public:
    ~Shader();
    SDL_GPUShader* handle() const { return shader_; }
    ShaderStage stage() const { return stage_; }
    u32 samplers() const { return samplers_; }
    u32 uniform_buffers() const { return uniform_buffers_; }
    // Unique per shader object (never reused), for pipeline caches.
    u32 id() const { return id_; }

private:
    friend class GpuDevice;
    Shader() = default;
    GpuDevice* device_ = nullptr;
    SDL_GPUShader* shader_ = nullptr;
    ShaderStage stage_ = ShaderStage::Vertex;
    u32 samplers_ = 0, uniform_buffers_ = 0, id_ = 0;
};

// A compute shader on the GPU, ready to dispatch (Renderer::dispatch). Loaded as an asset like Shader, from a
// ".comp.glsl" source: "<path>" -> "<path>.phsh", or the .glsl itself in dev builds. Resources follow SDL_GPU's
// compute layout: set 0 = sampled textures and `readonly` storage, set 1 = read-write storage, set 2 = uniforms.
class ComputeShader : NonCopyable {
public:
    ~ComputeShader();
    SDL_GPUComputePipeline* handle() const { return pipeline_; }
    // Threads per group (local_size_x/y/z), to work out how many groups cover a job.
    u32 threads(int axis) const { return threads_[axis]; }
    u32 readonly_buffers() const { return readonly_buffers_; }
    u32 readwrite_buffers() const { return readwrite_buffers_; }

private:
    friend class GpuDevice;
    ComputeShader() = default;
    GpuDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pipeline_ = nullptr;
    u32 threads_[3] = {1, 1, 1};
    u32 readonly_buffers_ = 0, readwrite_buffers_ = 0;
};

// Memory on the GPU that compute shaders read and write, and that sprites can be drawn from directly
// (Renderer::submit with a GpuBuffer).
class GpuBuffer : NonCopyable {
public:
    ~GpuBuffer();
    SDL_GPUBuffer* handle() const { return buffer_; }
    u32 size() const { return size_; }
    // Replaces bytes from `offset` (outside a frame's drawing, like texture uploads).
    bool upload(const void* data, u32 bytes, u32 offset = 0);

private:
    friend class GpuDevice;
    GpuBuffer() = default;
    GpuDevice* device_ = nullptr;
    SDL_GPUBuffer* buffer_ = nullptr;
    u32 size_ = 0;
};

struct GpuDeviceDesc {
    bool debug = PH_DEV_TOOLS; // backend validation layers
    bool vsync = true;
    // Force a backend ("vulkan", "metal", "direct3d12"); empty = SDL's best choice for the platform.
    std::string driver;
    // Direct3D 12: where shaders compiled from HLSL at load time are kept, so later runs start faster. Empty: none.
    std::filesystem::path shader_cache;
};

class GpuDevice : NonCopyable {
public:
    GpuDevice() = default;
    ~GpuDevice();

    bool init(SDL_Window* window, const GpuDeviceDesc& desc = {});
    void shutdown();

    SDL_GPUDevice* handle() const { return device_; }
    SDL_Window* window() const { return window_; }
    SDL_GPUTextureFormat swapchain_format() const;
    std::string_view driver() const { return driver_; }
    ShaderBlobFormat shader_format() const { return shader_format_; }
    void set_vsync(bool vsync);
    bool vsync() const { return vsync_; }

    std::unique_ptr<Texture> create_texture(const TextureDesc& desc);
    // Replaces all pixels of a texture (same size). Rebuilds mipmaps if it has them.
    bool update_texture(Texture& texture, const void* rgba8, bool premultiplied);
    // Replaces one rectangle. rgba8 points at the rectangle's top-left pixel in a source image `row_pixels` wide,
    // so a region of a larger CPU-side image can be sent without copying it out first.
    bool update_texture_region(Texture& texture, PixelRect rect, const void* rgba8, u32 row_pixels, bool premultiplied);
    std::unique_ptr<Shader> create_shader(const ShaderBundle& bundle, std::string_view name);
    std::unique_ptr<ComputeShader> create_compute_shader(const ShaderBundle& bundle, std::string_view name);
    // A zero-filled buffer of `bytes` for compute shaders and sprite drawing.
    std::unique_ptr<GpuBuffer> create_buffer(u32 bytes, std::string_view name);

    // Uploads raw bytes into a buffer through a temporary transfer buffer, on its own command buffer.
    bool upload(SDL_GPUBuffer* buffer, const void* data, u32 size, u32 offset = 0);

private:
    bool upload_pixels(SDL_GPUTexture* texture, PixelRect rect, const void* rgba8, u32 row_pixels, bool premultiplied,
                       bool mips);
    // The bundle's code for this device; Direct3D 12 compiles (or finds in the cache) DXBC from its HLSL.
    std::optional<ShaderBundle::Blob> code_for(const ShaderBundle& bundle, std::string_view name);

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    std::string driver_;
    ShaderBlobFormat shader_format_ = ShaderBlobFormat::SpirV;
    bool vsync_ = true;
    std::filesystem::path shader_cache_;
};

// Cached shader code on disk, by a hash of what it was made from. Used for D3D12's DXBC; works anywhere.
std::optional<std::vector<u8>> load_cached_shader(const std::filesystem::path& dir, u64 key);
void store_cached_shader(const std::filesystem::path& dir, u64 key, std::span<const u8> code);

// Registers Texture (PNG/JPG/TGA/BMP via stb_image), Shader, Font (.ttf/.otf), SpriteSheet (Aseprite JSON) and
// Tilemap (Tiled .tmj) loaders with the asset manager.
// default_filter: Nearest for pixel art, Linear (with mipmaps) for everything else.
void register_asset_loaders(AssetManager& assets, GpuDevice& device, Filter default_filter = Filter::Linear);

} // namespace ph::gfx
