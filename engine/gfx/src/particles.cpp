#include "bowl/gfx/particles.hpp"

#include "bowl/ecs/hierarchy.hpp"

#include <algorithm>
#include <cmath>

namespace bowl::gfx {

f32 ParticleSystem::random(f32 lo, f32 hi) {
    // xorshift64*: fast, deterministic per emitter.
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    u64 r = rng_ * 0x2545F4914F6CDD1Dull;
    f32 unit = static_cast<f32>(r >> 40) / static_cast<f32>(1u << 24);
    return lo + (hi - lo) * unit;
}

void ParticleSystem::spawn(const ParticleSettings& s, Vec2 origin) {
    if (age_.size() >= s.max_particles) return;
    Vec2 offset{random(-0.5f, 0.5f) * s.spawn_area.x, random(-0.5f, 0.5f) * s.spawn_area.y};
    f32 angle = s.direction + random(-s.spread, s.spread);
    position_.push_back(origin + offset);
    velocity_.push_back(Vec2::from_angle(angle) * random(s.speed_min, s.speed_max));
    age_.push_back(0);
    lifetime_.push_back(std::max(random(s.lifetime_min, s.lifetime_max), 1e-3f));
    rotation_.push_back(random(0, tau));
    spin_.push_back(random(s.spin_min, s.spin_max));
}

void ParticleSystem::kill(usize i) {
    // Swap-remove: order doesn't matter for particles.
    usize last = age_.size() - 1;
    position_[i] = position_[last];
    velocity_[i] = velocity_[last];
    age_[i] = age_[last];
    lifetime_[i] = lifetime_[last];
    rotation_[i] = rotation_[last];
    spin_[i] = spin_[last];
    position_.pop_back();
    velocity_.pop_back();
    age_.pop_back();
    lifetime_.pop_back();
    rotation_.pop_back();
    spin_.pop_back();
}

void ParticleSystem::burst(const ParticleSettings& s, Vec2 origin, u32 count) {
    for (u32 i = 0; i < count; ++i) spawn(s, origin);
}

void ParticleSystem::update(const ParticleSettings& s, Vec2 origin, f32 dt, bool emitting) {
    if (emitting && s.rate > 0) {
        spawn_debt_ += s.rate * dt;
        while (spawn_debt_ >= 1) {
            spawn(s, origin);
            spawn_debt_ -= 1;
        }
    } else {
        spawn_debt_ = 0;
    }
    f32 keep = std::max(0.0f, 1.0f - s.drag * dt);
    for (usize i = 0; i < age_.size();) {
        age_[i] += dt;
        if (age_[i] >= lifetime_[i]) {
            kill(i);
            continue;
        }
        velocity_[i] = (velocity_[i] + s.gravity * dt) * keep;
        position_[i] += velocity_[i] * dt;
        rotation_[i] += spin_[i] * dt;
        ++i;
    }
}

void ParticleSystem::draw(SpriteBatch& batch, const Texture& texture, const ParticleSettings& s, f32 depth) const {
    for (usize i = 0; i < age_.size(); ++i) {
        f32 t = age_[i] / lifetime_[i];
        f32 size = lerp(s.size_start, s.size_end, t);
        SpriteInstance sprite;
        sprite.position = position_[i];
        sprite.size = {size, size};
        sprite.rotation = rotation_[i];
        sprite.color = lerp(s.color_start, s.color_end, t);
        sprite.depth = depth;
        batch.draw(texture, sprite);
    }
}

void ParticleSystem::clear() {
    position_.clear();
    velocity_.clear();
    age_.clear();
    lifetime_.clear();
    rotation_.clear();
    spin_.clear();
    spawn_debt_ = 0;
}

void update_particle_emitters(ecs::World& world, f32 dt) {
    world.each<ParticleEmitter>([&](ecs::Entity e, ParticleEmitter& emitter) {
        const auto* global = world.try_get<const ecs::GlobalTransform2D>(e);
        emitter.system.update(emitter.settings, global ? global->position : emitter.origin, dt, emitter.emitting);
    });
}

void draw_particle_emitters(ecs::World& world, SpriteBatch& batch, const Texture& texture) {
    world.each<const ParticleEmitter>([&](const ParticleEmitter& emitter) {
        emitter.system.draw(batch, texture, emitter.settings, emitter.depth);
    });
}

} // namespace bowl::gfx
