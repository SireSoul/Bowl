#include "ph/gfx/renderer.hpp"

#include "ph/core/assert.hpp"
#include "ph/core/log.hpp"
#include "ph/core/profiler.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace ph::gfx {

Renderer::~Renderer() { shutdown(); }

namespace {

std::unique_ptr<Texture> make_light_texture(GpuDevice& device) {
    constexpr u32 n = 128;
    std::vector<u8> px(n * n * 4);
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            f32 dx = (static_cast<f32>(x) + 0.5f) / n * 2 - 1, dy = (static_cast<f32>(y) + 0.5f) / n * 2 - 1;
            f32 d = std::min(1.0f, dx * dx + dy * dy);
            f32 a = (1 - d) * (1 - d); // smooth falloff, zero at the edge
            u8 v = static_cast<u8>(a * 255.0f + 0.5f);
            u8* p = &px[(y * n + x) * 4];
            p[0] = p[1] = p[2] = p[3] = v; // premultiplied white
        }
    return device.create_texture({.width = n, .height = n, .rgba8 = px.data(), .premultiplied = true, .name = "light"});
}

SDL_GPUTexture* make_target(SDL_GPUDevice* d, SDL_GPUTextureFormat format, u32 w, u32 h, const char* name) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = std::max(1u, w);
    info.height = std::max(1u, h);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(d, &info);
    if (t) SDL_SetGPUTextureName(d, t, name);
    else error("renderer: can't create render target '{}': {}", name, SDL_GetError());
    return t;
}

SDL_GPUColorTargetBlendState blend_state(BlendMode mode) {
    SDL_GPUColorTargetBlendState b{};
    b.enable_blend = mode != BlendMode::Opaque;
    b.color_blend_op = b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (mode) {
    case BlendMode::Alpha:
        b.src_color_blendfactor = b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        b.src_color_blendfactor = b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case BlendMode::Multiply: // premultiplied: src * dst + dst * (1 - src.a)
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Opaque: break;
    }
    return b;
}

} // namespace

bool Renderer::init(GpuDevice& device, AssetManager& assets) {
    device_ = &device;
    sprite_vs_ = assets.load<Shader>("engine/shaders/sprite.vert");
    sprite_fs_ = assets.load<Shader>("engine/shaders/sprite.frag");
    fullscreen_vs_ = assets.load<Shader>("engine/shaders/fullscreen.vert");
    final_fs_ = assets.load<Shader>("engine/shaders/post/final.frag");
    bloom_pre_fs_ = assets.load<Shader>("engine/shaders/post/bloom_prefilter.frag");
    bloom_down_fs_ = assets.load<Shader>("engine/shaders/post/bloom_down.frag");
    bloom_up_fs_ = assets.load<Shader>("engine/shaders/post/bloom_up.frag");

    SDL_GPUSamplerCreateInfo s{};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(device.handle(), &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    s.max_lod = 1000;
    linear_ = SDL_CreateGPUSampler(device.handle(), &s);

    const u8 white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    white_ = device.create_texture({.width = 1, .height = 1, .rgba8 = white, .premultiplied = true,
                                    .filter = Filter::Nearest, .name = "white"});
    black_ = device.create_texture({.width = 1, .height = 1, .rgba8 = black, .premultiplied = true,
                                    .filter = Filter::Nearest, .name = "black"});
    light_ = make_light_texture(device);

    if (!nearest_ || !linear_ || !white_ || !black_ || !light_) {
        error("renderer: failed to create samplers/textures: {}", SDL_GetError());
        return false;
    }
    if (!sprite_vs_ || !sprite_fs_ || !fullscreen_vs_ || !final_fs_) {
        error("renderer: engine shaders failed to load; check the errors above");
        return false;
    }
    return true;
}

void Renderer::release_targets() {
    SDL_GPUDevice* d = device_ ? device_->handle() : nullptr;
    if (!d) return;
    for (SDL_GPUTexture** t : {&scene_, &final_, &lightmap_})
        if (*t) SDL_ReleaseGPUTexture(d, *t), *t = nullptr;
    for (auto& t : bloom_)
        if (t) SDL_ReleaseGPUTexture(d, t), t = nullptr;
    target_w_ = target_h_ = 0;
}

void Renderer::shutdown() {
    if (!device_) return;
    SDL_GPUDevice* d = device_->handle();
    if (d) {
        SDL_WaitForGPUIdle(d);
        for (auto& [key, p] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(d, p.pipeline);
        if (nearest_) SDL_ReleaseGPUSampler(d, nearest_);
        if (linear_) SDL_ReleaseGPUSampler(d, linear_);
        if (sprite_buffer_) SDL_ReleaseGPUBuffer(d, sprite_buffer_);
        if (sprite_transfer_) SDL_ReleaseGPUTransferBuffer(d, sprite_transfer_);
        release_targets();
    }
    pipelines_.clear();
    nearest_ = linear_ = nullptr;
    sprite_buffer_ = nullptr;
    sprite_transfer_ = nullptr;
    white_.reset();
    black_.reset();
    light_.reset();
    sprite_vs_ = sprite_fs_ = fullscreen_vs_ = final_fs_ = bloom_pre_fs_ = bloom_down_fs_ = bloom_up_fs_ = {};
    device_ = nullptr;
}

SDL_GPUGraphicsPipeline* Renderer::pipeline(const Shader& vs, const Shader& fs, SDL_GPUTextureFormat format,
                                            BlendMode blend) {
    PipelineKey key{vs.id(), fs.id(), format, blend};
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
        it->second.last_used = frame_;
        return it->second.pipeline;
    }
    SDL_GPUColorTargetDescription target{};
    target.format = format;
    target.blend_state = blend_state(blend);
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs.handle();
    info.fragment_shader = fs.handle();
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_->handle(), &info);
    if (!p) {
        error("renderer: SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        return nullptr;
    }
    pipelines_[key] = {p, frame_};
    return p;
}

bool Renderer::ensure_targets(u32 w, u32 h) {
    if (w == target_w_ && h == target_h_ && scene_ && final_) return true;
    SDL_WaitForGPUIdle(device_->handle());
    release_targets();
    SDL_GPUDevice* d = device_->handle();
    scene_ = make_target(d, hdr_format, w, h, "scene");
    final_ = make_target(d, device_->swapchain_format(), w, h, "final");
    lightmap_ = make_target(d, hdr_format, (w + 1) / 2, (h + 1) / 2, "light map");
    for (int i = 0; i < bloom_levels; ++i) {
        bloom_w_[i] = std::max(1u, w >> (i + 1));
        bloom_h_[i] = std::max(1u, h >> (i + 1));
        bloom_[i] = make_target(d, hdr_format, bloom_w_[i], bloom_h_[i], "bloom");
    }
    target_w_ = w;
    target_h_ = h;
    return scene_ && final_ && lightmap_ && std::all_of(std::begin(bloom_), std::end(bloom_), [](auto* t) { return t; });
}

bool Renderer::begin_frame() {
    PH_ASSERT(!cmd_, "begin_frame called twice");
    ++frame_;
    // Drop pipelines nobody has used for a while (old versions of hot-reloaded shaders, retired materials).
    if (frame_ % 300 == 0)
        std::erase_if(pipelines_, [&](const auto& kv) {
            if (frame_ - kv.second.last_used < 600) return false;
            SDL_ReleaseGPUGraphicsPipeline(device_->handle(), kv.second.pipeline);
            return true;
        });
    frame_sprites_.clear();
    draws_.clear();
    passes_.clear();
    light_passes_.clear();
    ui_passes_.clear();
    stats_ = {};

    cmd_ = SDL_AcquireGPUCommandBuffer(device_->handle());
    if (!cmd_) {
        error("renderer: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return false;
    }
    Uint32 w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, device_->window(), &swapchain_, &w, &h)) {
        error("renderer: swapchain acquire failed: {}", SDL_GetError());
        swapchain_ = nullptr;
    }
    target_size_ = {static_cast<f32>(w), static_cast<f32>(h)};
    return swapchain_ != nullptr;
}

void Renderer::record(const SpriteBatch& batch, std::vector<Pass>& passes) {
    if (!swapchain_ || batch.sprites_.empty()) return;
    usize n = batch.sprites_.size();
    // Sort indices, never the sprites themselves.
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0u);
    auto tex_id = [&](u32 i) { return batch.textures_[i] ? batch.textures_[i]->id() : 0u; };
    switch (batch.sort_) {
    case SpriteSort::None: break;
    case SpriteSort::Texture:
        std::stable_sort(order_.begin(), order_.end(), [&](u32 a, u32 b) { return tex_id(a) < tex_id(b); });
        break;
    case SpriteSort::Depth:
        std::stable_sort(order_.begin(), order_.end(), [&](u32 a, u32 b) {
            f32 da = batch.sprites_[a].depth, db = batch.sprites_[b].depth;
            return da != db ? da < db : tex_id(a) < tex_id(b);
        });
        break;
    }
    Pass pass{batch.camera_.view_projection(), static_cast<u32>(draws_.size()), 0, batch.blend_, batch.material_};
    u32 base = static_cast<u32>(frame_sprites_.size());
    frame_sprites_.reserve(frame_sprites_.size() + n);
    for (u32 k = 0; k < n; ++k) {
        u32 i = order_[k];
        const Texture* t = batch.textures_[i] ? batch.textures_[i] : white_.get();
        frame_sprites_.push_back(batch.sprites_[i]);
        if (draws_.size() > pass.first_draw && draws_.back().texture == t) ++draws_.back().count;
        else draws_.push_back({base + k, 1, t});
    }
    pass.draw_count = static_cast<u32>(draws_.size()) - pass.first_draw;
    passes.push_back(pass);
    stats_.sprites += static_cast<u32>(n);
    ++stats_.batches;
}

void Renderer::submit(const SpriteBatch& batch) { record(batch, passes_); }

void Renderer::submit_ui(const SpriteBatch& batch) { record(batch, ui_passes_); }

void Renderer::submit_light(const SpriteBatch& batch) {
    record(batch, light_passes_);
    if (!light_passes_.empty()) light_passes_.back().blend = BlendMode::Additive;
}

void Renderer::draw_sprites(SDL_GPURenderPass* pass, const std::vector<Pass>& passes, SDL_GPUTextureFormat format) {
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const Pass& p : passes) {
        const Shader* fs = sprite_fs_.get();
        if (p.material && p.material->fragment) fs = p.material->fragment.get();
        SDL_GPUGraphicsPipeline* pipe = pipeline(*sprite_vs_, *fs, format, p.blend);
        if (!pipe) continue;
        if (pipe != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &sprite_buffer_, 1);
            bound = pipe;
        }
        SDL_PushGPUVertexUniformData(cmd_, 0, p.view_projection.data(), sizeof(Mat4));
        if (p.material && !p.material->uniforms.empty() && fs->uniform_buffers() > 0)
            SDL_PushGPUFragmentUniformData(cmd_, 0, p.material->uniforms.data(),
                                           static_cast<u32>(p.material->uniforms.size()));
        u32 sampler_count = std::clamp(fs->samplers(), 1u, 4u);
        for (u32 i = p.first_draw; i < p.first_draw + p.draw_count; ++i) {
            const Draw& draw = draws_[i];
            SDL_GPUTextureSamplerBinding bindings[4];
            bindings[0] = {draw.texture->handle(), draw.texture->filter() == Filter::Nearest ? nearest_ : linear_};
            for (u32 b = 1; b < sampler_count; ++b) {
                const Texture* extra = p.material ? p.material->textures[b - 1] : nullptr;
                if (!extra) extra = white_.get();
                bindings[b] = {extra->handle(), extra->filter() == Filter::Nearest ? nearest_ : linear_};
            }
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, sampler_count);
            SDL_DrawGPUPrimitives(pass, draw.count * 6, 1, draw.first * 6, 0);
            ++stats_.draw_calls;
        }
    }
}

void Renderer::fullscreen(SDL_GPUTexture* target, bool clear, const Shader& fragment, SDL_GPUTextureFormat format,
                          BlendMode blend, std::span<const SDL_GPUTexture* const> inputs, const void* uniforms,
                          u32 size, SDL_GPURenderPass** keep_open) {
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    color.clear_color = {0, 0, 0, 0};
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);
    if (SDL_GPUGraphicsPipeline* pipe = pipeline(*fullscreen_vs_, fragment, format, blend)) {
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_GPUTextureSamplerBinding bindings[4];
        u32 n = static_cast<u32>(std::min<usize>(inputs.size(), 4));
        for (u32 i = 0; i < n; ++i) bindings[i] = {const_cast<SDL_GPUTexture*>(inputs[i]), linear_};
        if (n) SDL_BindGPUFragmentSamplers(pass, 0, bindings, n);
        if (uniforms && size) SDL_PushGPUFragmentUniformData(cmd_, 0, uniforms, size);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        ++stats_.draw_calls;
    }
    if (keep_open) *keep_open = pass;
    else SDL_EndGPURenderPass(pass);
}

void Renderer::run_bloom() {
    if (!bloom_pre_fs_ || !bloom_down_fs_ || !bloom_up_fs_) return;
    PH_PROFILE_SCOPE("bloom");
    f32 params[4] = {1.0f / static_cast<f32>(target_w_), 1.0f / static_cast<f32>(target_h_), post_.bloom_threshold,
                     post_.bloom_knee};
    const SDL_GPUTexture* scene_input[1] = {scene_};
    fullscreen(bloom_[0], true, *bloom_pre_fs_, hdr_format, BlendMode::Opaque, scene_input, params, sizeof params);
    for (int i = 1; i < bloom_levels; ++i) {
        f32 p[4] = {1.0f / static_cast<f32>(bloom_w_[i - 1]), 1.0f / static_cast<f32>(bloom_h_[i - 1]), 0, 0};
        const SDL_GPUTexture* in[1] = {bloom_[i - 1]};
        fullscreen(bloom_[i], true, *bloom_down_fs_, hdr_format, BlendMode::Opaque, in, p, sizeof p);
    }
    for (int i = bloom_levels - 1; i > 0; --i) {
        f32 p[4] = {1.0f / static_cast<f32>(bloom_w_[i]), 1.0f / static_cast<f32>(bloom_h_[i]), post_.bloom_radius, 0};
        const SDL_GPUTexture* in[1] = {bloom_[i]};
        fullscreen(bloom_[i - 1], false, *bloom_up_fs_, hdr_format, BlendMode::Additive, in, p, sizeof p);
    }
}

void Renderer::end_frame() {
    PH_PROFILE_SCOPE("render");
    if (!cmd_) return;
    SDL_GPUDevice* d = device_->handle();

    // --- upload every sprite of the frame at once -----------------------------------------------------------------
    u32 count = static_cast<u32>(frame_sprites_.size());
    if (swapchain_ && count > 0) {
        if (count > sprite_capacity_) {
            u32 cap = std::max<u32>(1024, sprite_capacity_);
            while (cap < count) cap *= 2;
            if (sprite_buffer_) SDL_ReleaseGPUBuffer(d, sprite_buffer_);
            if (sprite_transfer_) SDL_ReleaseGPUTransferBuffer(d, sprite_transfer_);
            SDL_GPUBufferCreateInfo binfo{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, cap * u32(sizeof(SpriteInstance)), 0};
            sprite_buffer_ = SDL_CreateGPUBuffer(d, &binfo);
            SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, binfo.size, 0};
            sprite_transfer_ = SDL_CreateGPUTransferBuffer(d, &tinfo);
            sprite_capacity_ = cap;
        }
        u32 bytes = count * u32(sizeof(SpriteInstance));
        // cycle = true: the GPU may still read last frame's data, SDL hands us fresh memory.
        void* mapped = SDL_MapGPUTransferBuffer(d, sprite_transfer_, true);
        std::memcpy(mapped, frame_sprites_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(d, sprite_transfer_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
        SDL_GPUTransferBufferLocation src{sprite_transfer_, 0};
        SDL_GPUBufferRegion dst{sprite_buffer_, 0, bytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
    }

    if (swapchain_ && overlay_.prepare) overlay_.prepare(cmd_);

    u32 w = static_cast<u32>(target_size_.x), h = static_cast<u32>(target_size_.y);
    SDL_GPUTransferBuffer* readback = nullptr;
    if (swapchain_ && w && h && ensure_targets(w, h)) {
        // Scene (HDR).
        SDL_GPUColorTargetInfo color{};
        color.texture = scene_;
        color.clear_color = {clear_color_.r, clear_color_.g, clear_color_.b, clear_color_.a};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);
        if (count > 0) draw_sprites(pass, passes_, hdr_format);
        SDL_EndGPURenderPass(pass);

        // Light map.
        if (post_.lighting) {
            SDL_GPUColorTargetInfo light{};
            light.texture = lightmap_;
            light.clear_color = {0, 0, 0, 0};
            light.load_op = SDL_GPU_LOADOP_CLEAR;
            light.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* lp = SDL_BeginGPURenderPass(cmd_, &light, 1, nullptr);
            if (count > 0 && !light_passes_.empty()) draw_sprites(lp, light_passes_, hdr_format);
            SDL_EndGPURenderPass(lp);
        }

        if (post_.bloom) run_bloom();

        // Final pass into the displayed image, then the overlay on top.
        struct {
            f32 grade[4], tint[4], ambient[4], vignette[4];
        } u = {{post_.exposure, post_.contrast, post_.saturation, post_.bloom ? post_.bloom_intensity : 0.0f},
               {post_.tint.r, post_.tint.g, post_.tint.b, 1},
               {post_.ambient.r, post_.ambient.g, post_.ambient.b, post_.lighting ? 1.0f : 0.0f},
               {post_.vignette, post_.vignette_radius, post_.vignette_softness, post_.tonemap ? 1.0f : 0.0f}};
        const SDL_GPUTexture* inputs[3] = {scene_, post_.bloom ? bloom_[0] : black_->handle(),
                                           post_.lighting ? lightmap_ : black_->handle()};
        SDL_GPURenderPass* final_pass = nullptr;
        fullscreen(final_, true, *final_fs_, device_->swapchain_format(), BlendMode::Opaque, inputs, &u, sizeof u,
                   &final_pass);
        if (count > 0 && !ui_passes_.empty()) draw_sprites(final_pass, ui_passes_, device_->swapchain_format());
        if (overlay_.draw) overlay_.draw(cmd_, final_pass);
        SDL_EndGPURenderPass(final_pass);

        if (capture_) {
            SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
            readback = SDL_CreateGPUTransferBuffer(d, &tinfo);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
            SDL_GPUTextureRegion src{};
            src.texture = final_;
            src.w = w;
            src.h = h;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{readback, 0, w, h};
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
        }

        SDL_GPUBlitInfo blit{};
        blit.source.texture = final_;
        blit.source.w = w;
        blit.source.h = h;
        blit.destination.texture = swapchain_;
        blit.destination.w = w;
        blit.destination.h = h;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd_, &blit);
    }
    stats_.pipelines = static_cast<u32>(pipelines_.size());

    if (readback) {
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
        SDL_WaitForGPUFences(d, true, &fence, 1);
        SDL_ReleaseGPUFence(d, fence);
        auto* pixels = static_cast<u8*>(SDL_MapGPUTransferBuffer(d, readback, false));
        std::vector<u8> rgba(pixels, pixels + w * h * 4);
        SDL_UnmapGPUTransferBuffer(d, readback);
        SDL_ReleaseGPUTransferBuffer(d, readback);
        SDL_GPUTextureFormat f = device_->swapchain_format();
        if (f == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || f == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
            for (usize i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        auto capture = std::move(capture_);
        capture_ = nullptr;
        capture(w, h, rgba);
    } else {
        SDL_SubmitGPUCommandBuffer(cmd_);
    }
    cmd_ = nullptr;
    swapchain_ = nullptr;
    last_stats_ = stats_;
}

} // namespace ph::gfx
