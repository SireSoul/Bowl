// Shared helpers for engine and game shaders: #include "common.glsl"

vec4 premultiply(vec4 c) { return vec4(c.rgb * c.a, c.a); }

vec2 rotate(vec2 v, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec2(v.x * c - v.y * s, v.x * s + v.y * c);
}
