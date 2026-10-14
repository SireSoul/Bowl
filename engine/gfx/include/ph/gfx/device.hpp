#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/math.hpp"
#include "ph/core/shader_bundle.hpp"
#include "ph/core/types.hpp"

#include <SDL3/SDL_gpu.h>

#include <memory>
#include <string>
#include <string_view>

struct SDL_Window;

namespace ph::gfx {

class GpuDevice;

enum class Filter : u8 { Nearest, Linear };

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

private:
    friend class GpuDevice;
    Shader() = default;
    GpuDevice* device_ = nullptr;
    SDL_GPUShader* shader_ = nullptr;
    ShaderStage stage_ = ShaderStage::Vertex;
};

struct GpuDeviceDesc {
    bool debug = PH_DEV_TOOLS; // backend validation layers
    bool vsync = true;
    // Force a backend ("vulkan", "metal", "direct3d12"); empty = SDL's best choice for the platform.
    std::string driver;
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
    // Replaces all pixels of a texture (same size), e.g. a growing glyph atlas. Rebuilds mipmaps if it has them.
    bool update_texture(Texture& texture, const void* rgba8, bool premultiplied);
    std::unique_ptr<Shader> create_shader(const ShaderBundle& bundle, std::string_view name);

    // Uploads raw bytes into a buffer through a temporary transfer buffer, on its own command buffer.
    bool upload(SDL_GPUBuffer* buffer, const void* data, u32 size, u32 offset = 0);

private:
    bool upload_pixels(SDL_GPUTexture* texture, u32 width, u32 height, const void* rgba8, bool premultiplied, bool mips);

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    std::string driver_;
    ShaderBlobFormat shader_format_ = ShaderBlobFormat::SpirV;
    bool vsync_ = true;
};

// Registers Texture (PNG/JPG/TGA/BMP via stb_image), Shader and Font (.ttf/.otf) loaders with the asset manager.
// default_filter: Nearest for pixel art, Linear (with mipmaps) for everything else.
void register_asset_loaders(AssetManager& assets, GpuDevice& device, Filter default_filter = Filter::Linear);

} // namespace ph::gfx
