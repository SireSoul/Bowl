#include "ph/gfx/device.hpp"

#include "ph/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <vector>

namespace ph::gfx {

namespace {
std::atomic<u32> next_texture_id{1};
}

Texture::~Texture() {
    if (texture_ && device_ && device_->handle()) SDL_ReleaseGPUTexture(device_->handle(), texture_);
}

Shader::~Shader() {
    if (shader_ && device_ && device_->handle()) SDL_ReleaseGPUShader(device_->handle(), shader_);
}

GpuDevice::~GpuDevice() { shutdown(); }

bool GpuDevice::init(SDL_Window* window, const GpuDeviceDesc& desc) {
    window_ = window;
    vsync_ = desc.vsync;
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;
    device_ = SDL_CreateGPUDevice(formats, desc.debug, desc.driver.empty() ? nullptr : desc.driver.c_str());
    if (!device_ && desc.debug) {
        warn("gpu: no device with validation layers ({}); retrying without", SDL_GetError());
        device_ = SDL_CreateGPUDevice(formats, false, desc.driver.empty() ? nullptr : desc.driver.c_str());
    }
    if (!device_) {
        error("gpu: SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }
    driver_ = SDL_GetGPUDeviceDriver(device_);
    SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device_);
    shader_format_ = (supported & SDL_GPU_SHADERFORMAT_MSL) ? ShaderBlobFormat::Msl : ShaderBlobFormat::SpirV;

    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        error("gpu: SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return false;
    }
    set_vsync(vsync_);
    info("gpu: {} backend, {} shaders", driver_, shader_format_ == ShaderBlobFormat::Msl ? "MSL" : "SPIR-V");
    return true;
}

void GpuDevice::shutdown() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);
    if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
}

SDL_GPUTextureFormat GpuDevice::swapchain_format() const { return SDL_GetGPUSwapchainTextureFormat(device_, window_); }

void GpuDevice::set_vsync(bool vsync) {
    vsync_ = vsync;
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!vsync) {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
        else if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

std::unique_ptr<Texture> GpuDevice::create_texture(const TextureDesc& desc) {
    if (!desc.width || !desc.height || !desc.rgba8) {
        error("gpu: texture '{}' has no pixels", desc.name);
        return nullptr;
    }
    bool mips = desc.filter == Filter::Linear && desc.mipmaps && std::max(desc.width, desc.height) > 1;
    u32 levels = mips ? static_cast<u32>(std::bit_width(std::max(desc.width, desc.height))) : 1;

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (mips ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
    if (!texture) {
        error("gpu: SDL_CreateGPUTexture '{}' failed: {}", desc.name, SDL_GetError());
        return nullptr;
    }
    if (!desc.name.empty()) SDL_SetGPUTextureName(device_, texture, std::string(desc.name).c_str());
    upload_pixels(texture, desc.width, desc.height, desc.rgba8, desc.premultiplied, mips);

    auto t = std::unique_ptr<Texture>(new Texture());
    t->device_ = this;
    t->texture_ = texture;
    t->width_ = desc.width;
    t->height_ = desc.height;
    t->filter_ = desc.filter;
    t->mipmapped_ = mips;
    t->id_ = next_texture_id.fetch_add(1, std::memory_order_relaxed);
    return t;
}

bool GpuDevice::update_texture(Texture& texture, const void* rgba8, bool premultiplied) {
    return upload_pixels(texture.texture_, texture.width_, texture.height_, rgba8, premultiplied, texture.mipmapped_);
}

bool GpuDevice::upload_pixels(SDL_GPUTexture* texture, u32 width, u32 height, const void* rgba8, bool premultiplied,
                              bool mips) {
    u32 bytes = width * height * 4;
    SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    if (!transfer) return false;
    auto* dst = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (premultiplied) {
        std::memcpy(dst, rgba8, bytes);
    } else {
        const auto* src = static_cast<const u8*>(rgba8);
        for (u32 i = 0; i < bytes; i += 4) {
            u32 a = src[i + 3];
            dst[i + 0] = static_cast<u8>((src[i + 0] * a + 127) / 255);
            dst[i + 1] = static_cast<u8>((src[i + 1] * a + 127) / 255);
            dst[i + 2] = static_cast<u8>((src[i + 2] * a + 127) / 255);
            dst[i + 3] = static_cast<u8>(a);
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{transfer, 0, width, height};
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src_info, &region, false);
    SDL_EndGPUCopyPass(copy);
    if (mips) SDL_GenerateMipmapsForGPUTexture(cmd, texture);
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

std::unique_ptr<Shader> GpuDevice::create_shader(const ShaderBundle& bundle, std::string_view name) {
    const ShaderBundle::Blob* blob = bundle.find(shader_format_);
    if (!blob) {
        error("gpu: shader '{}' has no code for the {} backend", name, driver_);
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info{};
    info.code = blob->code.data();
    info.code_size = blob->code.size();
    info.entrypoint = blob->entry_point.c_str();
    info.format = shader_format_ == ShaderBlobFormat::Msl ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = bundle.stage == ShaderStage::Fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    info.num_samplers = bundle.samplers;
    info.num_storage_textures = bundle.storage_textures;
    info.num_storage_buffers = bundle.storage_buffers;
    info.num_uniform_buffers = bundle.uniform_buffers;
    // MSL source must be NUL-terminated.
    std::vector<u8> msl_source;
    if (info.format == SDL_GPU_SHADERFORMAT_MSL) {
        msl_source.assign(blob->code.begin(), blob->code.end());
        msl_source.push_back(0);
        info.code = msl_source.data();
        info.code_size = msl_source.size() - 1;
    }
    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) {
        error("gpu: SDL_CreateGPUShader '{}' failed: {}", name, SDL_GetError());
        return nullptr;
    }
    auto s = std::unique_ptr<Shader>(new Shader());
    s->device_ = this;
    s->shader_ = shader;
    s->stage_ = bundle.stage;
    return s;
}

bool GpuDevice::upload(SDL_GPUBuffer* buffer, const void* data, u32 size, u32 offset) {
    SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    if (!transfer) return false;
    std::memcpy(SDL_MapGPUTransferBuffer(device_, transfer, false), data, size);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{transfer, 0};
    SDL_GPUBufferRegion dst{buffer, offset, size};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

} // namespace ph::gfx
