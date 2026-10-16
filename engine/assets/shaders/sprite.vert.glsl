#version 450
// Vertex pulling: no vertex buffers. Each sprite is 6 vertices; the vertex index picks the sprite and the corner,
// and the sprite's data comes from a storage buffer. Must match bowl::gfx::SpriteInstance.
// Draws always start at vertex 0 and pass their first sprite in the uniform block: backends disagree on whether the
// vertex index includes the draw's first-vertex offset, so the shader never relies on it.
#include "common.glsl"

struct Sprite {
    vec2 position;
    vec2 size;
    vec2 origin;   // pivot in 0..1 of the size
    float rotation;
    float depth;
    vec4 uv;       // u0, v0, u1, v1
    vec4 color;    // straight alpha; premultiplied here
};

layout(std430, set = 0, binding = 0) readonly buffer Sprites { Sprite sprites[]; };
layout(std140, set = 1, binding = 0) uniform Camera {
    mat4 view_projection;
    uvec4 draw; // x: index of this draw's first sprite
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));

void main() {
    uint vertex = uint(gl_VertexIndex);
    Sprite s = sprites[draw.x + vertex / 6u];
    vec2 corner = corners[vertex % 6u];
    vec2 local = (corner - s.origin) * s.size;
    vec2 world = s.position + rotate(local, s.rotation);
    gl_Position = view_projection * vec4(world, 0.0, 1.0);
    v_uv = mix(s.uv.xy, s.uv.zw, corner);
    v_color = premultiply(s.color);
}
