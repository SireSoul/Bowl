#version 450
// GPU particles (gfx::GpuParticles): one thread per particle. Particles live in a ring: each frame the CPU names the
// slots to fill with new ones, and every other living particle moves. Each thread also writes its particle's sprite,
// which the renderer draws straight from the buffer.

layout(local_size_x = 64) in;

struct Particle {
    vec2 position;
    vec2 velocity;
    float age;
    float life;
    float size_scale;
    float spin;
};

struct Sprite { // bowl::gfx::SpriteInstance
    vec2 position;
    vec2 size;
    vec2 origin;
    float rotation;
    float depth;
    vec4 uv;
    vec4 color;
};

layout(std430, set = 1, binding = 0) buffer Particles { Particle particles[]; };
layout(std430, set = 1, binding = 1) buffer Sprites { Sprite sprites[]; };

layout(std140, set = 2, binding = 0) uniform Settings {
    vec4 emitter;     // xy: position, z: spawn radius, w: dt
    vec4 motion;      // xy: velocity, z: random speed, w: drag
    vec4 forces;      // xy: gravity, z: shortest life, w: longest life
    vec4 sizes;       // x: start size, y: end size, z: depth, w: random seed
    vec4 color_start;
    vec4 color_end;
    uvec4 spawn;      // x: first slot to fill, y: how many, z: capacity, w: 1 to clear everything
};

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float random(inout uint state) {
    state = hash(state);
    return float(state & 0xFFFFFFu) / 16777216.0;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint capacity = spawn.z;
    if (i >= capacity) return;
    Particle p = particles[i];
    float dt = emitter.w;
    if (spawn.w != 0u) p.life = 0.0;

    uint slot = (i + capacity - spawn.x) % capacity;
    if (slot < spawn.y) {
        uint state = hash(i * 747796405u + floatBitsToUint(sizes.w));
        float angle = random(state) * 6.2831853;
        float dist = sqrt(random(state)) * emitter.z;
        p.position = emitter.xy + vec2(cos(angle), sin(angle)) * dist;
        float kick_angle = random(state) * 6.2831853;
        p.velocity = motion.xy + vec2(cos(kick_angle), sin(kick_angle)) * motion.z * random(state);
        p.age = 0.0;
        p.life = mix(forces.z, forces.w, random(state));
        p.size_scale = 0.75 + 0.5 * random(state);
        p.spin = (random(state) - 0.5) * 4.0;
    } else if (p.age < p.life) {
        p.velocity += forces.xy * dt;
        p.velocity *= exp(-motion.w * dt);
        p.position += p.velocity * dt;
        p.age += dt;
    }
    particles[i] = p;

    bool alive = p.age < p.life;
    float t = alive ? clamp(p.age / max(p.life, 1e-4), 0.0, 1.0) : 1.0;
    Sprite s;
    s.position = p.position;
    s.size = vec2(alive ? mix(sizes.x, sizes.y, t) * p.size_scale : 0.0);
    s.origin = vec2(0.5);
    s.rotation = p.spin * p.age;
    s.depth = sizes.z;
    s.uv = vec4(0.0, 0.0, 1.0, 1.0);
    s.color = mix(color_start, color_end, t);
    sprites[i] = s;
}
