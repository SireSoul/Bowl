#include "ph/gfx/gpu_particles.hpp"

#include "ph/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace ph::gfx {

namespace {
// Must match engine/assets/shaders/particles.comp.glsl.
constexpr u32 particle_bytes = 32;
struct Uniforms {
    f32 emitter[4], motion[4], forces[4], sizes[4], color_start[4], color_end[4];
    u32 spawn[4];
};
} // namespace

bool GpuParticles::init(GpuDevice& device, AssetManager& assets, u32 capacity) {
    capacity_ = std::max(capacity, 1u);
    shader_ = assets.load<ComputeShader>("engine/shaders/particles.comp");
    particles_ = device.create_buffer(capacity_ * particle_bytes, "gpu particles");
    sprites_ = device.create_buffer(capacity_ * static_cast<u32>(sizeof(SpriteInstance)), "gpu particle sprites");
    head_ = burst_ = 0;
    carry_ = 0;
    return shader_ && particles_ && sprites_;
}

void GpuParticles::update(Renderer& renderer, f32 dt, const GpuParticleSettings& s) {
    if (!shader_ || !particles_) return;
    carry_ += std::max(s.rate, 0.0f) * std::max(dt, 0.0f);
    u32 spawn = static_cast<u32>(carry_);
    carry_ -= static_cast<f32>(spawn);
    spawn = std::min(spawn + burst_, capacity_);
    burst_ = 0;

    Uniforms u{};
    auto set = [](f32 (&dst)[4], f32 a, f32 b, f32 c, f32 d) {
        dst[0] = a;
        dst[1] = b;
        dst[2] = c;
        dst[3] = d;
    };
    set(u.emitter, s.position.x, s.position.y, s.radius, dt);
    set(u.motion, s.velocity.x, s.velocity.y, s.speed_spread, s.drag);
    set(u.forces, s.gravity.x, s.gravity.y, std::min(s.life_min, s.life_max), std::max(s.life_min, s.life_max));
    f32 seed = static_cast<f32>(++frame_ % 1000003);
    set(u.sizes, s.size_start, s.size_end, s.depth, seed);
    set(u.color_start, s.color_start.r, s.color_start.g, s.color_start.b, s.color_start.a);
    set(u.color_end, s.color_end.r, s.color_end.g, s.color_end.b, s.color_end.a);
    u.spawn[0] = head_;
    u.spawn[1] = spawn;
    u.spawn[2] = capacity_;
    u.spawn[3] = clear_ ? 1u : 0u;
    head_ = (head_ + spawn) % capacity_;
    clear_ = false;

    ComputeDispatch job;
    job.shader = shader_.get();
    job.write = {particles_.get(), sprites_.get()};
    job.set_uniforms(u);
    job.cover(capacity_);
    renderer.dispatch(std::move(job));
}

void GpuParticles::draw(Renderer& renderer, const Camera2D& camera, const Texture* texture, BlendMode blend) const {
    if (sprites_) renderer.submit(*sprites_, capacity_, texture, camera, blend);
}

} // namespace ph::gfx
