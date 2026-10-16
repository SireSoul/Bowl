#pragma once

#include "bowl/gfx/renderer.hpp"

#include <memory>

namespace bowl::gfx {

// How a GpuParticles emitter behaves. Change it every frame if you like.
struct GpuParticleSettings {
    f32 rate = 2000;          // new particles per second
    Vec2 position;            // the emitter, in the world
    f32 radius = 0;           // new particles start anywhere within this circle
    Vec2 velocity{0, -120};   // starting velocity
    f32 speed_spread = 80;    // plus up to this much speed in a random direction
    Vec2 gravity{0, 150};
    f32 drag = 0.5f;          // velocity lost per second (exponential)
    f32 life_min = 1, life_max = 2;
    f32 size_start = 8, size_end = 0;
    Color color_start = Color::white();
    Color color_end = Color::white().with_alpha(0);
    f32 depth = 0;            // every particle's sprite depth
};

// Particles simulated and drawn entirely on the GPU: hundreds of thousands at the cost of one compute dispatch and one
// draw a frame, with nothing sent from the CPU but the settings. Use the CPU ParticleEmitter for effects the game
// reads back or that must be deterministic; use these for spectacle (sparks, snow, magic, fireworks).
class GpuParticles : NonCopyable {
public:
    bool init(GpuDevice& device, AssetManager& assets, u32 capacity);
    // Queues this frame's step: spawns rate * dt new particles (plus any burst) and moves the rest.
    void update(Renderer& renderer, f32 dt, const GpuParticleSettings& settings);
    // Draws them with one texture (a soft dot, a spark). Call in the same frame, after update.
    void draw(Renderer& renderer, const Camera2D& camera, const Texture* texture,
              BlendMode blend = BlendMode::Additive) const;
    // Adds `count` particles at the next update, on top of the rate.
    void burst(u32 count) { burst_ += count; }
    // Removes every particle at the next update.
    void clear() { clear_ = true; }
    u32 capacity() const { return capacity_; }

private:
    Asset<ComputeShader> shader_;
    std::unique_ptr<GpuBuffer> particles_, sprites_;
    u32 capacity_ = 0, head_ = 0, burst_ = 0, frame_ = 0;
    f32 carry_ = 0; // fractions of a particle owed from earlier frames
    bool clear_ = false;
};

} // namespace bowl::gfx
