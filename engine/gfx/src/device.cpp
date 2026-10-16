#include "bowl/gfx/device.hpp"

#include "bowl/core/assert.hpp"
#include "bowl/core/file.hpp"
#include "bowl/core/log.hpp"
#include "bowl/core/type_id.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <vector>

namespace bowl::gfx {

namespace {
std::atomic<u32> next_texture_id{1};

const char* hlsl_profile(ShaderStage stage) {
    return stage == ShaderStage::Fragment ? "ps_5_1" : stage == ShaderStage::Compute ? "cs_5_1" : "vs_5_1";
}

// D3D12 takes DXBC; the HLSL compiler (d3dcompiler_47.dll) ships with Windows, so shaders compile on the player's
// machine at load time (and are cached on disk: see GpuDevice::code_for).
std::optional<std::vector<u8>> compile_hlsl_to_dxbc([[maybe_unused]] const ShaderBundle::Blob& hlsl,
                                                    [[maybe_unused]] ShaderStage stage,
                                                    [[maybe_unused]] std::string_view name, std::string& err) {
#ifdef _WIN32
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    std::string source_name(name);
    HRESULT hr = D3DCompile(hlsl.code.data(), hlsl.code.size(), source_name.c_str(), nullptr, nullptr,
                            hlsl.entry_point.c_str(), hlsl_profile(stage), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                            &errors);
    if (errors) {
        err.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(hr) || !code) {
        if (code) code->Release();
        if (err.empty()) err = "D3DCompile failed";
        return std::nullopt;
    }
    const auto* bytes = static_cast<const u8*>(code->GetBufferPointer());
    std::vector<u8> out(bytes, bytes + code->GetBufferSize());
    code->Release();
    return out;
#else
    err = "HLSL can only be compiled on Windows";
    return std::nullopt;
#endif
}
} // namespace

Texture::~Texture() {
    if (texture_ && device_ && device_->handle()) SDL_ReleaseGPUTexture(device_->handle(), texture_);
}

Shader::~Shader() {
    if (shader_ && device_ && device_->handle()) SDL_ReleaseGPUShader(device_->handle(), shader_);
}

ComputeShader::~ComputeShader() {
    if (pipeline_ && device_ && device_->handle()) SDL_ReleaseGPUComputePipeline(device_->handle(), pipeline_);
}

GpuBuffer::~GpuBuffer() {
    if (buffer_ && device_ && device_->handle()) SDL_ReleaseGPUBuffer(device_->handle(), buffer_);
}

bool GpuBuffer::upload(const void* data, u32 bytes, u32 offset) {
    if (offset + bytes > size_) return false;
    return device_->upload(buffer_, data, bytes, offset);
}

// Shader cache

static std::filesystem::path cache_file(const std::filesystem::path& dir, u64 key) {
    return dir / std::format("{:016x}.bin", key);
}

std::optional<std::vector<u8>> load_cached_shader(const std::filesystem::path& dir, u64 key) {
    if (dir.empty()) return std::nullopt;
    auto bytes = read_file_bytes(cache_file(dir, key));
    // Stored with the key in front, so a file renamed or cut short is never mistaken for the right code.
    if (!bytes || bytes->size() < 8) return std::nullopt;
    u64 stored = 0;
    std::memcpy(&stored, bytes->data(), 8);
    if (stored != key) return std::nullopt;
    return std::vector<u8>(bytes->begin() + 8, bytes->end());
}

void store_cached_shader(const std::filesystem::path& dir, u64 key, std::span<const u8> code) {
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<u8> bytes(8 + code.size());
    std::memcpy(bytes.data(), &key, 8);
    std::memcpy(bytes.data() + 8, code.data(), code.size());
    if (!write_file_bytes(cache_file(dir, key), bytes.data(), bytes.size()))
        warn("gpu: can't write the shader cache in {}", dir.string());
}

GpuDevice::~GpuDevice() { shutdown(); }

bool GpuDevice::init(SDL_Window* window, const GpuDeviceDesc& desc) {
    window_ = window;
    vsync_ = desc.vsync;
    shader_cache_ = desc.shader_cache;
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;
#ifdef _WIN32
    formats |= SDL_GPU_SHADERFORMAT_DXBC; // compiled at runtime from the bundle's HLSL (see create_shader)
#endif
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
    if (driver_ == "metal") shader_format_ = ShaderBlobFormat::Msl;
    else if (driver_ == "direct3d12") shader_format_ = ShaderBlobFormat::Dxbc;
    else shader_format_ = ShaderBlobFormat::SpirV;

    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        error("gpu: SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return false;
    }
    set_vsync(vsync_);
    const char* format_name = shader_format_ == ShaderBlobFormat::Msl    ? "MSL"
                              : shader_format_ == ShaderBlobFormat::Dxbc ? "DXBC (from HLSL)"
                                                                         : "SPIR-V";
    info("gpu: {} backend, {} shaders", driver_, format_name);
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
    upload_pixels(texture, {0, 0, desc.width, desc.height}, desc.rgba8, desc.width, desc.premultiplied, mips);

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
    return upload_pixels(texture.texture_, {0, 0, texture.width_, texture.height_}, rgba8, texture.width_,
                         premultiplied, texture.mipmapped_);
}

bool GpuDevice::update_texture_region(Texture& texture, PixelRect rect, const void* rgba8, u32 row_pixels,
                                      bool premultiplied) {
    if (rect.w == 0 || rect.h == 0) return true;
    BOWL_ASSERT(rect.x + rect.w <= texture.width_ && rect.y + rect.h <= texture.height_, "region outside the texture");
    return upload_pixels(texture.texture_, rect, rgba8, row_pixels, premultiplied, texture.mipmapped_);
}

bool GpuDevice::upload_pixels(SDL_GPUTexture* texture, PixelRect rect, const void* rgba8, u32 row_pixels,
                              bool premultiplied, bool mips) {
    u32 row_bytes = rect.w * 4;
    SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, row_bytes * rect.h, 0};
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    if (!transfer) return false;
    auto* dst = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    for (u32 row = 0; row < rect.h; ++row) {
        const u8* src = static_cast<const u8*>(rgba8) + static_cast<usize>(row) * row_pixels * 4;
        u8* out = dst + static_cast<usize>(row) * row_bytes;
        if (premultiplied) {
            std::memcpy(out, src, row_bytes);
            continue;
        }
        for (u32 i = 0; i < row_bytes; i += 4) {
            u32 a = src[i + 3];
            out[i + 0] = static_cast<u8>((src[i + 0] * a + 127) / 255);
            out[i + 1] = static_cast<u8>((src[i + 1] * a + 127) / 255);
            out[i + 2] = static_cast<u8>((src[i + 2] * a + 127) / 255);
            out[i + 3] = static_cast<u8>(a);
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info{transfer, 0, rect.w, rect.h};
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.x = rect.x;
    region.y = rect.y;
    region.w = rect.w;
    region.h = rect.h;
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &src_info, &region, false);
    SDL_EndGPUCopyPass(copy);
    if (mips) SDL_GenerateMipmapsForGPUTexture(cmd, texture);
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

std::optional<ShaderBundle::Blob> GpuDevice::code_for(const ShaderBundle& bundle, std::string_view name) {
    if (const ShaderBundle::Blob* blob = bundle.find(shader_format_)) return *blob;
    if (shader_format_ == ShaderBlobFormat::Dxbc)
        if (const ShaderBundle::Blob* hlsl = bundle.find(ShaderBlobFormat::Hlsl)) {
            // Cached by the HLSL text and the profile, so a changed shader never picks up old code.
            std::string_view text(reinterpret_cast<const char*>(hlsl->code.data()), hlsl->code.size());
            u64 key = fnv1a(text) ^ (fnv1a(hlsl_profile(bundle.stage)) * 31);
            if (auto cached = load_cached_shader(shader_cache_, key))
                return ShaderBundle::Blob{ShaderBlobFormat::Dxbc, hlsl->entry_point, std::move(*cached)};
            std::string err;
            if (auto dxbc = compile_hlsl_to_dxbc(*hlsl, bundle.stage, name, err)) {
                store_cached_shader(shader_cache_, key, *dxbc);
                return ShaderBundle::Blob{ShaderBlobFormat::Dxbc, hlsl->entry_point, std::move(*dxbc)};
            }
            error("gpu: HLSL for shader '{}' didn't compile: {}", name, err);
            return std::nullopt;
        }
    error("gpu: shader '{}' has no code for the {} backend (rebuild shaders?)", name, driver_);
    return std::nullopt;
}

std::unique_ptr<ComputeShader> GpuDevice::create_compute_shader(const ShaderBundle& bundle, std::string_view name) {
    if (bundle.stage != ShaderStage::Compute) {
        error("gpu: '{}' isn't a compute shader", name);
        return nullptr;
    }
    auto blob = code_for(bundle, name);
    if (!blob) return nullptr;
    SDL_GPUComputePipelineCreateInfo info{};
    std::vector<u8> code = blob->code;
    info.format = shader_format_ == ShaderBlobFormat::Msl    ? SDL_GPU_SHADERFORMAT_MSL
                  : shader_format_ == ShaderBlobFormat::Dxbc ? SDL_GPU_SHADERFORMAT_DXBC
                                                             : SDL_GPU_SHADERFORMAT_SPIRV;
    if (info.format == SDL_GPU_SHADERFORMAT_MSL) code.push_back(0); // MSL source must be NUL-terminated
    info.code = code.data();
    info.code_size = blob->code.size();
    info.entrypoint = blob->entry_point.c_str();
    info.num_samplers = bundle.samplers;
    info.num_readonly_storage_textures = bundle.storage_textures;
    info.num_readonly_storage_buffers = bundle.storage_buffers;
    info.num_readwrite_storage_textures = bundle.readwrite_storage_textures;
    info.num_readwrite_storage_buffers = bundle.readwrite_storage_buffers;
    info.num_uniform_buffers = bundle.uniform_buffers;
    info.threadcount_x = bundle.threads[0];
    info.threadcount_y = bundle.threads[1];
    info.threadcount_z = bundle.threads[2];
    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
    if (!pipeline) {
        error("gpu: SDL_CreateGPUComputePipeline '{}' failed: {}", name, SDL_GetError());
        return nullptr;
    }
    auto c = std::unique_ptr<ComputeShader>(new ComputeShader());
    c->device_ = this;
    c->pipeline_ = pipeline;
    for (int i = 0; i < 3; ++i) c->threads_[i] = bundle.threads[i];
    c->readonly_buffers_ = bundle.storage_buffers;
    c->readwrite_buffers_ = bundle.readwrite_storage_buffers;
    return c;
}

std::unique_ptr<GpuBuffer> GpuDevice::create_buffer(u32 bytes, std::string_view name) {
    bytes = std::max(bytes, 16u);
    SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                     SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                 bytes, 0};
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
    if (!buffer) {
        error("gpu: buffer '{}' ({} bytes) failed: {}", name, bytes, SDL_GetError());
        return nullptr;
    }
    SDL_SetGPUBufferName(device_, buffer, std::string(name).c_str());
    auto b = std::unique_ptr<GpuBuffer>(new GpuBuffer());
    b->device_ = this;
    b->buffer_ = buffer;
    b->size_ = bytes;
    std::vector<u8> zeros(bytes, 0);
    upload(buffer, zeros.data(), bytes);
    return b;
}

std::unique_ptr<Shader> GpuDevice::create_shader(const ShaderBundle& bundle, std::string_view name) {
    if (bundle.stage == ShaderStage::Compute) {
        error("gpu: '{}' is a compute shader; load it as a ComputeShader", name);
        return nullptr;
    }
    auto code = code_for(bundle, name);
    if (!code) return nullptr;
    const ShaderBundle::Blob* blob = &*code;
    SDL_GPUShaderCreateInfo info{};
    info.code = blob->code.data();
    info.code_size = blob->code.size();
    info.entrypoint = blob->entry_point.c_str();
    info.format = shader_format_ == ShaderBlobFormat::Msl    ? SDL_GPU_SHADERFORMAT_MSL
                  : shader_format_ == ShaderBlobFormat::Dxbc ? SDL_GPU_SHADERFORMAT_DXBC
                                                             : SDL_GPU_SHADERFORMAT_SPIRV;
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
    s->samplers_ = bundle.samplers;
    s->uniform_buffers_ = bundle.uniform_buffers;
    s->id_ = next_texture_id.fetch_add(1, std::memory_order_relaxed); // shared counter: ids just need to be unique
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

} // namespace bowl::gfx
