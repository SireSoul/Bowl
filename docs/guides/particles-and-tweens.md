# Particles and tweens

Two small tools for motion: CPU particle systems for sparks, smoke, dust and magic, and tweens for animating values
along easing curves.

## Particles

A particle system spawns many short-lived sprites according to a `gfx::ParticleSettings`:

```cpp
#include "ph/gfx/particles.hpp"

gfx::ParticleSettings smoke;
smoke.rate = 20;                       // per second while emitting
smoke.lifetime_min = 1.5f;
smoke.lifetime_max = 3.0f;
smoke.speed_min = 10;
smoke.speed_max = 30;
smoke.direction = -pi / 2;             // up (y points down)
smoke.spread = 0.4f;                   // radians either side
smoke.gravity = {0, -8};               // drifts upward a little faster over time
smoke.drag = 0.3f;
smoke.size_start = 12;
smoke.size_end = 40;
smoke.color_start = Color::hex(0x8a8f96, 0.5f);
smoke.color_end = Color::hex(0x8a8f96, 0.0f);
```

| Setting | Default | |
| --- | --- | --- |
| `rate` | 30 | Particles per second while emitting |
| `lifetime_min`, `lifetime_max` | 0.6, 1.2 | Seconds, picked at random per particle |
| `speed_min`, `speed_max` | 40, 120 | Initial speed |
| `direction` | -pi/2 (up) | Radians |
| `spread` | 0.6 | Radians either side of `direction`; `pi` is every direction |
| `spawn_area` | (0, 0) | Spawn anywhere in a box this size around the origin |
| `gravity` | (0, 0) | Acceleration |
| `drag` | 0 | Fraction of velocity lost per second |
| `size_start`, `size_end` | 8, 2 | Interpolated over each particle's life |
| `color_start`, `color_end` | white to transparent white | Interpolated likewise; values above 1 bloom |
| `spin_min`, `spin_max` | 0, 0 | Rotation speed, radians per second |
| `max_particles` | 2000 | Cap; spawning stops at the cap |

`ParticleSettings` is reflected, so it saves with the world and is editable in the F1 inspector. Tune effects live,
then copy the numbers back into code.

### On an entity

The usual way is the `gfx::ParticleEmitter` component, which pairs settings with the live particles. It emits from
the entity's `GlobalTransform2D` if it has one, otherwise from its `origin` field:

```cpp
ecs::Entity chimney = world.create(ecs::Transform2D{{320, 140}}, gfx::ParticleEmitter{.settings = smoke});

// every frame, in update():
gfx::update_particle_emitters(engine.world(), dt);

// in render(), into a batch of their own:
puffs.begin(engine.camera());
gfx::draw_particle_emitters(engine.world(), puffs, engine.renderer().light_texture());
engine.renderer().submit(puffs);
```

`emitter.emitting = false` stops new particles (existing ones live out their lives). For one-off bursts, call the
system directly: `emitter.system.burst(emitter.settings, where, 30)`. `emitter.depth` sets the depth of its sprites.

Particles are saved without their live state: a loaded world's emitters start empty and fill up again.

### On its own

`gfx::ParticleSystem` can be used without the ECS:

```cpp
gfx::ParticleSystem sparks;                         // or ParticleSystem(seed) for a specific sequence

sparks.burst(settings, hit_point, 40);              // spawn 40 now
sparks.update(settings, origin, dt, /*emitting*/ true);
sparks.draw(batch, engine.renderer().light_texture(), settings, depth);
usize alive = sparks.size();
sparks.clear();
```

### Look

- Use `renderer.light_texture()` for soft round particles, or any texture you like.
- Additive batches (`BlendMode::Additive`) suit fire, sparks and magic; alpha batches suit smoke and dust.
- Over-bright colors (above 1) glow when bloom is on.

### Determinism and cost

Each system has its own seeded random generator, so the same seed and the same calls give the same particles.
Particles are simulated on the CPU, stored as parallel arrays and drawn as sprites: thousands are cheap, but they're
visual effects, not physics.

### GPU particles

For hundreds of thousands of particles, `gfx::GpuParticles` simulates and draws them entirely on the GPU: one compute
dispatch and one draw call a frame, with nothing sent from the CPU but the settings.

```cpp
#include "ph/gfx/gpu_particles.hpp"

gfx::GpuParticles fountain;
fountain.init(engine.gpu(), engine.assets(), 200000);   // capacity

// render, every frame
gfx::GpuParticleSettings s{.rate = 60000, .position = {0, 200}, .velocity = {0, -520}, .speed_spread = 160,
                           .gravity = {0, 420}, .life_min = 1.5f, .life_max = 3, .size_start = 5, .size_end = 1,
                           .color_start = {0.5f, 0.8f, 2.0f, 1}, .color_end = {0.9f, 0.3f, 1.4f, 0}};
fountain.update(engine.renderer(), engine.delta(), s);
fountain.draw(engine.renderer(), engine.camera(), &engine.renderer().light_texture());
```

`burst(n)` adds particles at the next update and `clear()` removes them all. When the emitter makes more particles
than its capacity holds, the oldest are reused. All particles share one texture and one depth, and draw in
submission order with the scene. They're not deterministic and can't be read back by game code; use
`ParticleEmitter` for that. The sprites sample shows a fountain with `--gpu-particles`.

## Easing and tweens

`ph/core/ease.hpp` has the standard easing curves, each mapping 0..1 to 0..1:

```cpp
f32 t = ease(Ease::OutBack, 0.5f);
```

`Linear`, and `In`, `Out` and `InOut` variants of `Quad`, `Cubic`, `Quart`, `Sine`, `Expo`, `Back` (overshoots),
`Elastic` (springs) and `Bounce`.

`Tween<T>` animates a value from one point to another with a curve. It works with anything `lerp` handles: `f32`,
`Vec2`, `Color` and more.

```cpp
Tween<Vec2> slide{{-300, 40}, {40, 40}, 0.45f, Ease::OutBack};      // from, to, seconds, curve
Tween<Color> fade{Color::white(), Color::white().with_alpha(0), 1.0f, Ease::InQuad, 0, 2.0f}; // 2 s delay

// every frame:
panel_position = slide.update(dt);
if (slide.done()) { /* ... */ }
```

| | |
| --- | --- |
| `update(dt)` | Advance and return the current value |
| `value()` | The current value without advancing |
| `done()` | Past the delay and the duration |
| `restart()` | Back to the beginning |
| `reverse()` | Play back to the start from wherever it is now, such as a menu closing halfway through opening |

Tweens are plain structs, so they can be components or live inside one; step them in a system. They aren't
reflected, so a component holding one can't be listed in `PH_COMPONENT`; recreate tweens after loading instead.
