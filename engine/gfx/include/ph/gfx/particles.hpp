#pragma once

#include "ph/core/math.hpp"
#include "ph/core/reflect.hpp"
#include "ph/ecs/world.hpp"
#include "ph/gfx/sprites.hpp"

#include <vector>

namespace ph::gfx {

// What an emitter spawns. Reflected: tune it live in the inspector, save it with the world.
struct ParticleSettings {
    f32 rate = 30;              // particles per second while emitting
    f32 lifetime_min = 0.6f, lifetime_max = 1.2f;
    f32 speed_min = 40, speed_max = 120;
    f32 direction = -pi / 2;    // radians; -pi/2 is up (y-down world)
    f32 spread = 0.6f;          // radians either side of direction
    Vec2 spawn_area;            // spawn within this box around the emitter (0 = a point)
    Vec2 gravity{0, 0};
    f32 drag = 0;               // fraction of velocity lost per second
    f32 size_start = 8, size_end = 2;
    Color color_start = Color::white();
    Color color_end = Color::white().with_alpha(0);
    f32 spin_min = 0, spin_max = 0; // radians per second
    u32 max_particles = 2000;
};
PH_REFLECT(ParticleSettings, rate, lifetime_min, lifetime_max, speed_min, speed_max, direction, spread, spawn_area,
           gravity, drag, size_start, size_end, color_start, color_end, spin_min, spin_max, max_particles)

// The live particles of one emitter, stored as parallel arrays. Positions are in world space, so moving the emitter
// leaves a trail.
class ParticleSystem {
public:
    ParticleSystem() : ParticleSystem(0x9E3779B97F4A7C15ull) {}
    explicit ParticleSystem(u64 seed) : rng_(seed | 1) {}

    // Spawns `count` particles at `origin` right away (explosions, puffs).
    void burst(const ParticleSettings& s, Vec2 origin, u32 count);
    // Advances everything by dt, spawning at s.rate while `emitting`.
    void update(const ParticleSettings& s, Vec2 origin, f32 dt, bool emitting = true);
    // Draws each particle as `texture` (light_texture() makes soft round particles).
    void draw(SpriteBatch& batch, const Texture& texture, const ParticleSettings& s, f32 depth = 0) const;

    usize size() const { return age_.size(); }
    void clear();

private:
    f32 random(f32 lo, f32 hi);
    void spawn(const ParticleSettings& s, Vec2 origin);
    void kill(usize i);

    std::vector<Vec2> position_, velocity_;
    std::vector<f32> age_, lifetime_, rotation_, spin_;
    f32 spawn_debt_ = 0;
    u64 rng_;
};

// A particle emitter as a component, for effects that belong to an entity (a torch, a chimney, a spell). It emits
// from the entity's GlobalTransform2D, or from `origin` if it has none. Settings save with the world; the live
// particles don't.
//
//     world.create(Transform2D{{40, 80}}, ParticleEmitter{.settings = smoke});
//     update_particle_emitters(world, dt);                    // in update() or a system
//     draw_particle_emitters(world, batch, renderer.light_texture());
struct ParticleEmitter {
    ParticleSettings settings;
    bool emitting = true;
    Vec2 origin;
    f32 depth = 0;
    ParticleSystem system;
};
PH_COMPONENT(ParticleEmitter, settings, emitting, origin, depth)

void update_particle_emitters(ecs::World& world, f32 dt);
void draw_particle_emitters(ecs::World& world, SpriteBatch& batch, const Texture& texture);

} // namespace ph::gfx
