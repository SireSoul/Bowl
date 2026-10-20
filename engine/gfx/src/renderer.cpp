#include "bowl/gfx/renderer.hpp"

#include "bowl/core/assert.hpp"
#include "bowl/core/log.hpp"
#include "bowl/core/profiler.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace bowl::gfx {

Renderer::~Renderer() { shutdown(); }

Material light_material(AssetManager& assets, const LightLook& look) {
    struct Params {
        f32 height, normals, shadows, pad;
    };
    Material m;
    m.fragment = assets.load<Shader>("engine/shaders/light.frag");
    m.set_uniforms(Params{std::max(look.height, 0.01f), look.normals ? 1.0f : 0.0f, look.shadows ? 1.0f : 0.0f, 0});
    return m;
}

std::vector<u8> make_neutral_lut(u32 size) {
    size = std::clamp(size, 2u, 64u);
    std::vector<u8> px(static_cast<usize>(size) * size * size * 4);
    auto level = [&](u32 i) { return static_cast<u8>((i * 255 + (size - 1) / 2) / (size - 1)); };
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size * size; ++x) {
            u8* p = &px[(static_cast<usize>(y) * size * size + x) * 4];
            p[0] = level(x % size); // red across each slice
            p[1] = level(y);        // green down
            p[2] = level(x / size); // blue by slice
            p[3] = 255;
        }
    return px;
}

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
    composite_fs_ = assets.load<Shader>("engine/shaders/post/composite.frag");

    SDL_GPUSamplerCreateInfo s{};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(device.handle(), &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    s.max_lod = 1000;
    linear_ = SDL_CreateGPUSampler(device.handle(), &s);

    const u8 white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255}, clear[4] = {0, 0, 0, 0},
             flat[4] = {128, 128, 255, 255};
    auto pixel = [&](const u8* rgba, const char* name) {
        return device.create_texture(
            {.width = 1, .height = 1, .rgba8 = rgba, .premultiplied = true, .filter = Filter::Nearest, .name = name});
    };
    white_ = pixel(white, "white");
    black_ = pixel(black, "black");
    clear_ = pixel(clear, "transparent");
    flat_normal_ = pixel(flat, "flat normal");
    light_ = make_light_texture(device);

    if (!nearest_ || !linear_ || !white_ || !black_ || !clear_ || !flat_normal_ || !light_) {
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
    for (SDL_GPUTexture** t : {&scene_, &final_, &lightmap_, &scene2_, &emissive_, &normals_, &occluders_, &final2_})
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
    clear_.reset();
    flat_normal_.reset();
    light_.reset();
    sprite_vs_ = sprite_fs_ = fullscreen_vs_ = final_fs_ = bloom_pre_fs_ = bloom_down_fs_ = bloom_up_fs_ = {};
    composite_fs_ = {};
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
    BOWL_ASSERT(!cmd_, "begin_frame called twice");
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
    emissive_passes_.clear();
    normal_passes_.clear();
    occluder_passes_.clear();
    compute_.clear();
    stats_ = {};

    cmd_ = SDL_AcquireGPUCommandBuffer(device_->handle());
    if (!cmd_) {
        error("renderer: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return false;
    }
    device_->hold_releases(); // textures destroyed from here on may still be drawn by this frame
    Uint32 w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, device_->window(), &swapchain_, &w, &h)) {
        error("renderer: swapchain acquire failed: {}", SDL_GetError());
        swapchain_ = nullptr;
    }
    target_size_ = {static_cast<f32>(w), static_cast<f32>(h)};
    return swapchain_ != nullptr;
}

void Renderer::dispatch(ComputeDispatch job) {
    if (!cmd_ || !job.shader || !job.shader->handle()) return;
    compute_.push_back(std::move(job));
}

void Renderer::submit(const GpuBuffer& sprites, u32 count, const Texture* texture, const Camera2D& camera,
                      BlendMode blend, const Material* material) {
    count = std::min<u32>(count, sprites.size() / static_cast<u32>(sizeof(SpriteInstance)));
    if (!swapchain_ || count == 0) return;
    Pass pass{camera.view_projection(), static_cast<u32>(draws_.size()), 1, blend, material, sprites.handle()};
    draws_.push_back(draw_of(0, count, texture ? *texture : *white_));
    passes_.push_back(pass);
    stats_.sprites += count;
    ++stats_.batches;
}

void Renderer::record(const SpriteBatch& batch, std::vector<Pass>& passes) {
    if (!swapchain_ || batch.sprites_.empty()) return;
    usize n = batch.sprites_.size();
    sprite_order(batch, order_);
    Pass pass{batch.camera_.view_projection(), static_cast<u32>(draws_.size()), 0, batch.blend_, batch.material_};
    u32 base = static_cast<u32>(frame_sprites_.size());
    frame_sprites_.reserve(frame_sprites_.size() + n);
    for (u32 k = 0; k < n; ++k) {
        u32 i = order_[k];
        const Texture* t = batch.textures_[i] ? batch.textures_[i] : white_.get();
        const Material* m = batch.material_of(i);
        frame_sprites_.push_back(batch.sprites_[i]);
        if (draws_.size() > pass.first_draw && draws_.back().texture == t->handle() && draws_.back().filter == t->filter() &&
            draws_.back().material == m)
            ++draws_.back().count;
        else draws_.push_back(draw_of(base + k, 1, *t, m));
    }
    pass.draw_count = static_cast<u32>(draws_.size()) - pass.first_draw;
    passes.push_back(pass);
    stats_.sprites += static_cast<u32>(n);
    ++stats_.batches;
}

void Renderer::submit(const SpriteBatch& batch) { record(batch, passes_); }

void Renderer::submit_ui(const SpriteBatch& batch) { record(batch, ui_passes_); }

void Renderer::submit(const StaticSprites& sprites, const Camera2D& camera, BlendMode blend, const Material* material) {
    if (!swapchain_ || sprites.empty() || !sprites.buffer_) return;
    Pass pass{camera.view_projection(), static_cast<u32>(draws_.size()), 0, blend, material, sprites.buffer_};
    for (const StaticSprites::Run& run : sprites.runs_)
        draws_.push_back(draw_of(run.first, run.count, run.texture ? *run.texture : *white_, run.material));
    pass.draw_count = static_cast<u32>(sprites.runs_.size());
    passes_.push_back(pass);
    stats_.sprites += sprites.count_;
    ++stats_.batches;
}

void Renderer::submit_light(const SpriteBatch& batch) {
    record(batch, light_passes_);
    if (!light_passes_.empty()) light_passes_.back().blend = BlendMode::Additive;
}

void Renderer::submit_emissive(const SpriteBatch& batch) { record(batch, emissive_passes_); }
void Renderer::submit_normals(const SpriteBatch& batch) { record(batch, normal_passes_); }
void Renderer::submit_occluders(const SpriteBatch& batch) { record(batch, occluder_passes_); }

SDL_GPUTexture* Renderer::lazy_target(SDL_GPUTexture*& target, SDL_GPUTextureFormat format, const char* name) {
    if (!target) target = make_target(device_->handle(), format, target_w_, target_h_, name);
    return target;
}

void Renderer::draw_into(SDL_GPUTexture* target, Color clear, const std::vector<Pass>& passes,
                         SDL_GPUTextureFormat format, std::array<SDL_GPUTexture*, 3> extras) {
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = {clear.r, clear.g, clear.b, clear.a};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);
    if (!passes.empty()) draw_sprites(pass, passes, format, extras);
    SDL_EndGPURenderPass(pass);
}

std::span<const u8> Renderer::uniforms_for(const Shader& shader, const std::vector<u8>& bytes) {
    if (shader.uniform_buffers() == 0) return {};
    // Older bundles don't describe their block: the material's bytes as they are.
    if (shader.block_size() == 0) return bytes;
    uniform_scratch_ = shader.defaults();
    if (uniform_scratch_.size() < bytes.size()) uniform_scratch_.resize(bytes.size());
    std::memcpy(uniform_scratch_.data(), bytes.data(), bytes.size());
    if (shader.time_) {
        f32 t[4] = {static_cast<f32>(time_), time_delta_, static_cast<f32>(frame_ % 16777216), 0};
        ShaderBundle::write(uniform_scratch_, *shader.time_, t);
    }
    if (shader.screen_) {
        f32 w = std::max(target_size_.x, 1.0f), h = std::max(target_size_.y, 1.0f);
        f32 v[4] = {w, h, 1.0f / w, 1.0f / h};
        ShaderBundle::write(uniform_scratch_, *shader.screen_, v);
    }
    return uniform_scratch_;
}

void Renderer::draw_sprites(SDL_GPURenderPass* pass, const std::vector<Pass>& passes, SDL_GPUTextureFormat format,
                            std::array<SDL_GPUTexture*, 3> extras) {
    static const std::vector<u8> no_uniforms;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBuffer* bound_buffer = nullptr;
    for (const Pass& p : passes) {
        SDL_GPUBuffer* buffer = p.buffer ? p.buffer : sprite_buffer_;
        if (!buffer) continue;
        struct {
            Mat4 view_projection;
            u32 draw[4];
        } camera{p.view_projection, {0, 0, 0, 0}};
        const Material* pushed = nullptr; // whose uniforms the shader has now
        bool any_pushed = false;
        for (u32 i = p.first_draw; i < p.first_draw + p.draw_count; ++i) {
            const Draw& draw = draws_[i];
            // The sprites' own material, or the pass's.
            const Material* m = draw.material ? draw.material : p.material;
            const Shader* fs = m && m->fragment ? m->fragment.get() : sprite_fs_.get();
            BlendMode blend = m && m->blend && &passes != &light_passes_ ? *m->blend : p.blend;
            SDL_GPUGraphicsPipeline* pipe = pipeline(*sprite_vs_, *fs, format, blend);
            if (!pipe) continue;
            if (pipe != bound) {
                SDL_BindGPUGraphicsPipeline(pass, pipe);
                bound = pipe;
                bound_buffer = nullptr;
                any_pushed = false;
            }
            if (buffer != bound_buffer) {
                SDL_BindGPUVertexStorageBuffers(pass, 0, &buffer, 1);
                bound_buffer = buffer;
            }
            if (fs->uniform_buffers() > 0 && (!any_pushed || pushed != m)) {
                auto u = uniforms_for(*fs, m ? m->uniforms : no_uniforms);
                if (!u.empty()) SDL_PushGPUFragmentUniformData(cmd_, 0, u.data(), static_cast<u32>(u.size()));
                pushed = m;
                any_pushed = true;
            }
            u32 sampler_count = std::clamp(fs->samplers(), 1u, 4u);
            SDL_GPUTextureSamplerBinding bindings[4];
            bindings[0] = {draw.texture, draw.filter == Filter::Nearest ? nearest_ : linear_};
            for (u32 b = 1; b < sampler_count; ++b) {
                const Texture* extra = m ? m->textures[b - 1] : nullptr;
                if (extra) bindings[b] = {extra->handle(), extra->filter() == Filter::Nearest ? nearest_ : linear_};
                else if (extras[b - 1]) bindings[b] = {extras[b - 1], linear_};
                else bindings[b] = {white_->handle(), nearest_};
            }
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, sampler_count);
            camera.draw[0] = draw.first;
            SDL_PushGPUVertexUniformData(cmd_, 0, &camera, sizeof camera);
            SDL_DrawGPUPrimitives(pass, draw.count * 6, 1, 0, 0);
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

void Renderer::run_bloom(SDL_GPUTexture* input) {
    if (!bloom_pre_fs_ || !bloom_down_fs_ || !bloom_up_fs_) return;
    BOWL_PROFILE_SCOPE("bloom");
    f32 params[4] = {1.0f / static_cast<f32>(target_w_), 1.0f / static_cast<f32>(target_h_), post_.bloom_threshold,
                     post_.bloom_knee};
    const SDL_GPUTexture* scene_input[1] = {input};
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
    BOWL_PROFILE_SCOPE("render");
    if (!cmd_) return;
    SDL_GPUDevice* d = device_->handle();

    // Upload every sprite of the frame at once
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

    // Compute work, before anything that might draw what it writes.
    for (const ComputeDispatch& job : compute_) {
        std::vector<SDL_GPUStorageBufferReadWriteBinding> rw;
        for (GpuBuffer* b : job.write) rw.push_back({b->handle(), false, {}});
        SDL_GPUComputePass* pass =
            SDL_BeginGPUComputePass(cmd_, nullptr, 0, rw.data(), static_cast<u32>(rw.size()));
        SDL_BindGPUComputePipeline(pass, job.shader->handle());
        if (!job.read.empty()) {
            std::vector<SDL_GPUBuffer*> ro;
            for (const GpuBuffer* b : job.read) ro.push_back(b->handle());
            SDL_BindGPUComputeStorageBuffers(pass, 0, ro.data(), static_cast<u32>(ro.size()));
        }
        if (!job.uniforms.empty())
            SDL_PushGPUComputeUniformData(cmd_, 0, job.uniforms.data(), static_cast<u32>(job.uniforms.size()));
        if (job.groups[0] && job.groups[1] && job.groups[2])
            SDL_DispatchGPUCompute(pass, job.groups[0], job.groups[1], job.groups[2]);
        SDL_EndGPUComputePass(pass);
    }

    if (swapchain_ && overlay_.prepare) overlay_.prepare(cmd_);

    u32 w = static_cast<u32>(target_size_.x), h = static_cast<u32>(target_size_.y);
    SDL_GPUTransferBuffer* readback = nullptr;
    if (swapchain_ && w && h && ensure_targets(w, h)) {
        // Scene (HDR). Emissive sprites go in it too, so they bloom like any bright sprite.
        draw_into(scene_, clear_color_, passes_, hdr_format);
        if (!emissive_passes_.empty()) {
            SDL_GPUColorTargetInfo again{};
            again.texture = scene_;
            again.load_op = SDL_GPU_LOADOP_LOAD;
            again.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &again, 1, nullptr);
            draw_sprites(pass, emissive_passes_, hdr_format);
            SDL_EndGPURenderPass(pass);
        }
        SDL_GPUTexture* image = scene_; // the scene so far
        auto other_scene = [&] { return image == scene_ ? lazy_target(scene2_, hdr_format, "scene 2") : scene_; };

        // Light map, with normals and occluders for lights that use them.
        if (post_.lighting) {
            SDL_GPUTexture* normals = flat_normal_->handle();
            SDL_GPUTexture* occluders = clear_->handle();
            if (!normal_passes_.empty() && lazy_target(normals_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "normals")) {
                draw_into(normals_, Color{0.5f, 0.5f, 1, 1}, normal_passes_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
                normals = normals_;
            }
            if (!occluder_passes_.empty() &&
                lazy_target(occluders_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "occluders")) {
                draw_into(occluders_, Color::transparent(), occluder_passes_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
                occluders = occluders_;
            }
            draw_into(lightmap_, Color::transparent(), light_passes_, hdr_format, {normals, occluders, nullptr});
        }
        SDL_GPUTexture* emissive = nullptr;
        if (!emissive_passes_.empty() && lazy_target(emissive_, hdr_format, "emissive")) {
            draw_into(emissive_, Color::transparent(), emissive_passes_, hdr_format);
            emissive = emissive_;
        }
        // Lit scene: scene * (ambient + light), with emissive sprites over it.
        if ((post_.lighting || emissive) && composite_fs_ && other_scene()) {
            f32 u[4] = {post_.ambient.r, post_.ambient.g, post_.ambient.b, post_.lighting ? 1.0f : 0.0f};
            const SDL_GPUTexture* in[3] = {image, post_.lighting ? lightmap_ : black_->handle(),
                                           emissive ? emissive : clear_->handle()};
            SDL_GPUTexture* out = other_scene();
            fullscreen(out, true, *composite_fs_, hdr_format, BlendMode::Opaque, in, u, sizeof u);
            image = out;
        }
        // Bloom comes from the scene's own bright pixels (HDR sprites, emissive ones), not from where lights overlap.
        if (post_.bloom) run_bloom(scene_);

        // The game's effects, each reading the image so far and writing the other buffer.
        auto run_effects = [&](PostStage stage, SDL_GPUTexture*& current, auto next, SDL_GPUTextureFormat format) {
            for (const PostEffect& e : post_effects_) {
                if (!e.enabled || e.stage != stage || !e.material || !e.material->fragment) continue;
                SDL_GPUTexture* out = next();
                if (!out) return;
                const SDL_GPUTexture* in[4] = {current, white_->handle(), white_->handle(), white_->handle()};
                for (int i = 0; i < 3; ++i)
                    if (e.material->textures[i]) in[i + 1] = e.material->textures[i]->handle();
                auto uniforms = uniforms_for(*e.material->fragment, e.material->uniforms);
                usize samplers = std::clamp(e.material->fragment->samplers(), 1u, 4u);
                fullscreen(out, true, *e.material->fragment, format, BlendMode::Opaque, std::span(in, samplers),
                           uniforms.empty() ? nullptr : uniforms.data(), static_cast<u32>(uniforms.size()));
                current = out;
            }
        };
        run_effects(PostStage::Scene, image, other_scene, hdr_format);

        // Final pass into the displayed image: grading, LUT, lens, vignette.
        struct {
            f32 grade[4], tint[4], lens[4], vignette[4];
        } u = {{post_.exposure, post_.contrast, post_.saturation, post_.bloom ? post_.bloom_intensity : 0.0f},
               {post_.tint.r, post_.tint.g, post_.tint.b, post_.lut ? std::clamp(post_.lut_strength, 0.0f, 1.0f) : 0.0f},
               {post_.chromatic_aberration, post_.grain, static_cast<f32>(frame_ % 4096), 0},
               {post_.vignette, post_.vignette_radius, post_.vignette_softness, post_.tonemap ? 1.0f : 0.0f}};
        const SDL_GPUTexture* inputs[3] = {image, post_.bloom ? bloom_[0] : black_->handle(),
                                           post_.lut ? post_.lut->handle() : white_->handle()};
        SDL_GPUTextureFormat display = device_->swapchain_format();
        bool final_effects = std::any_of(post_effects_.begin(), post_effects_.end(), [](const PostEffect& e) {
            return e.enabled && e.stage == PostStage::Final && e.material && e.material->fragment;
        });
        SDL_GPURenderPass* final_pass = nullptr;
        fullscreen(final_, true, *final_fs_, display, BlendMode::Opaque, inputs, &u, sizeof u,
                   final_effects ? nullptr : &final_pass);
        SDL_GPUTexture* shown = final_;
        if (final_effects) {
            run_effects(PostStage::Final, shown,
                        [&] { return shown == final_ ? lazy_target(final2_, display, "final 2") : final_; }, display);
            SDL_GPUColorTargetInfo again{};
            again.texture = shown;
            again.load_op = SDL_GPU_LOADOP_LOAD;
            again.store_op = SDL_GPU_STOREOP_STORE;
            final_pass = SDL_BeginGPURenderPass(cmd_, &again, 1, nullptr);
        }
        // Then the UI and the overlay on top.
        if (!ui_passes_.empty()) draw_sprites(final_pass, ui_passes_, display);
        if (overlay_.draw) overlay_.draw(cmd_, final_pass);
        SDL_EndGPURenderPass(final_pass);

        if (capture_) {
            SDL_GPUTransferBufferCreateInfo tinfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
            readback = SDL_CreateGPUTransferBuffer(d, &tinfo);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
            SDL_GPUTextureRegion src{};
            src.texture = shown;
            src.w = w;
            src.h = h;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{readback, 0, w, h};
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
        }

        SDL_GPUBlitInfo blit{};
        blit.source.texture = shown;
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
    device_->let_go_releases(); // submitted: SDL frees them once the GPU is done with the frame
    cmd_ = nullptr;
    swapchain_ = nullptr;
    last_stats_ = stats_;
}

} // namespace bowl::gfx
