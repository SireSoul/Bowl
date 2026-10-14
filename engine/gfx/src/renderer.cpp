#include "ph/gfx/renderer.hpp"

#include "ph/core/assert.hpp"
#include "ph/core/log.hpp"
#include "ph/core/profiler.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <numeric>

namespace ph::gfx {

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(GpuDevice& device, AssetManager& assets) {
    device_ = &device;
    vertex_ = assets.load<Shader>("engine/shaders/sprite.vert");
    fragment_ = assets.load<Shader>("engine/shaders/sprite.frag");

    SDL_GPUSamplerCreateInfo s{};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(device.handle(), &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    s.max_lod = 1000;
    linear_ = SDL_CreateGPUSampler(device.handle(), &s);

    const u8 white[4] = {255, 255, 255, 255};
    white_ = device.create_texture({1, 1, white, true, Filter::Nearest, "white"});

    if (!nearest_ || !linear_ || !white_) {
        error("renderer: failed to create samplers/textures: {}", SDL_GetError());
        return false;
    }
    if (!rebuild_pipeline()) {
        error("renderer: sprite pipeline unavailable; check the shader errors above");
        return false;
    }
    return true;
}

void Renderer::shutdown() {
    if (!device_) return;
    SDL_GPUDevice* d = device_->handle();
    if (d) {
        SDL_WaitForGPUIdle(d);
        if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(d, pipeline_);
        if (nearest_) SDL_ReleaseGPUSampler(d, nearest_);
        if (linear_) SDL_ReleaseGPUSampler(d, linear_);
        if (sprite_buffer_) SDL_ReleaseGPUBuffer(d, sprite_buffer_);
        if (sprite_transfer_) SDL_ReleaseGPUTransferBuffer(d, sprite_transfer_);
        if (scene_) SDL_ReleaseGPUTexture(d, scene_);
    }
    scene_ = nullptr;
    pipeline_ = nullptr;
    nearest_ = linear_ = nullptr;
    sprite_buffer_ = nullptr;
    sprite_transfer_ = nullptr;
    white_.reset();
    vertex_ = {};
    fragment_ = {};
    device_ = nullptr;
}

bool Renderer::rebuild_pipeline() {
    if (!vertex_ || !fragment_) return false;
    SDL_GPUColorTargetDescription target{};
    target.format = device_->swapchain_format();
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex_->handle();
    info.fragment_shader = fragment_->handle();
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_->handle(), &info);
    if (!pipeline) {
        error("renderer: SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        return false;
    }
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_->handle(), pipeline_);
    pipeline_ = pipeline;
    vertex_version_ = vertex_.version();
    fragment_version_ = fragment_.version();
    return true;
}

bool Renderer::begin_frame() {
    PH_ASSERT(!cmd_, "begin_frame called twice");
    // Shaders hot-reloaded: rebuild the pipeline (keeping the old one if the new one fails).
    if (vertex_.version() != vertex_version_ || fragment_.version() != fragment_version_) {
        if (rebuild_pipeline()) info("renderer: sprite pipeline rebuilt");
        else {
            vertex_version_ = vertex_.version();
            fragment_version_ = fragment_.version();
        }
    }
    frame_sprites_.clear();
    draws_.clear();
    passes_.clear();
    stats_ = {};

    cmd_ = SDL_AcquireGPUCommandBuffer(device_->handle());
    if (!cmd_) {
        error("renderer: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return false;
    }
    Uint32 w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, device_->window(), &target_, &w, &h)) {
        error("renderer: swapchain acquire failed: {}", SDL_GetError());
        target_ = nullptr;
    }
    target_size_ = {static_cast<f32>(w), static_cast<f32>(h)};
    return target_ != nullptr;
}

void Renderer::submit(const SpriteBatch& batch) {
    if (!target_ || batch.sprites_.empty()) return;
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

    Pass pass{batch.camera_.view_projection(), static_cast<u32>(draws_.size()), 0};
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
    passes_.push_back(pass);
    stats_.sprites += static_cast<u32>(n);
    ++stats_.batches;
}

void Renderer::end_frame() {
    PH_PROFILE_SCOPE("render");
    if (!cmd_) return;
    SDL_GPUDevice* d = device_->handle();

    // --- upload every sprite of the frame at once -----------------------------------------------------------------
    u32 count = static_cast<u32>(frame_sprites_.size());
    if (target_ && count > 0) {
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

    if (target_ && overlay_.prepare) overlay_.prepare(cmd_);

    u32 w = static_cast<u32>(target_size_.x), h = static_cast<u32>(target_size_.y);
    if (target_ && (!scene_ || scene_w_ != w || scene_h_ != h)) {
        // The scene renders off-screen, then gets copied to the window: the hook for post-processing and capture.
        if (scene_) SDL_ReleaseGPUTexture(d, scene_);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = device_->swapchain_format();
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        scene_ = SDL_CreateGPUTexture(d, &info);
        scene_w_ = w;
        scene_h_ = h;
        if (scene_) SDL_SetGPUTextureName(d, scene_, "scene");
    }

    if (target_ && scene_) {
        SDL_GPUColorTargetInfo color{};
        color.texture = scene_;
        color.clear_color = {clear_color_.r, clear_color_.g, clear_color_.b, clear_color_.a};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);

        if (count > 0 && pipeline_) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &sprite_buffer_, 1);
            for (const Pass& p : passes_) {
                SDL_PushGPUVertexUniformData(cmd_, 0, p.view_projection.data(), sizeof(Mat4));
                for (u32 i = p.first_draw; i < p.first_draw + p.draw_count; ++i) {
                    const Draw& draw = draws_[i];
                    SDL_GPUTextureSamplerBinding binding{draw.texture->handle(),
                                                         draw.texture->filter() == Filter::Nearest ? nearest_ : linear_};
                    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                    SDL_DrawGPUPrimitives(pass, draw.count * 6, 1, draw.first * 6, 0);
                    ++stats_.draw_calls;
                }
            }
        }
        SDL_EndGPURenderPass(pass);

        // Screenshot: read the scene back (without the dev overlay).
        SDL_GPUTransferBuffer* readback = nullptr;
        if (capture_) {
            SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
            readback = SDL_CreateGPUTransferBuffer(d, &tinfo);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
            SDL_GPUTextureRegion src{};
            src.texture = scene_;
            src.w = w;
            src.h = h;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{readback, 0, w, h};
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
        }

        SDL_GPUBlitInfo blit{};
        blit.source.texture = scene_;
        blit.source.w = w;
        blit.source.h = h;
        blit.destination.texture = target_;
        blit.destination.w = w;
        blit.destination.h = h;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd_, &blit);

        if (overlay_.draw) {
            SDL_GPUColorTargetInfo over{};
            over.texture = target_;
            over.load_op = SDL_GPU_LOADOP_LOAD;
            over.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* overlay_pass = SDL_BeginGPURenderPass(cmd_, &over, 1, nullptr);
            overlay_.draw(cmd_, overlay_pass);
            SDL_EndGPURenderPass(overlay_pass);
        }

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
            cmd_ = nullptr;
            target_ = nullptr;
            return;
        }
    }

    SDL_SubmitGPUCommandBuffer(cmd_);
    cmd_ = nullptr;
    target_ = nullptr;
}

} // namespace ph::gfx
