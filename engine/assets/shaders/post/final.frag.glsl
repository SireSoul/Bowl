#version 450
// The last pass: bloom, color grading, lookup table, lens effects and vignette, from the lit HDR scene to the
// displayed image. With default settings it is an exact copy, so games that don't use any of it look unchanged.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(set = 2, binding = 1) uniform sampler2D u_bloom;
layout(set = 2, binding = 2) uniform sampler2D u_lut; // N*N x N strip; white 1x1 when there's none

layout(std140, set = 3, binding = 0) uniform Settings {
    vec4 grade;    // x: exposure, y: contrast, z: saturation, w: bloom intensity
    vec4 tint;     // rgb multiplier, a: LUT strength (0: no LUT)
    vec4 lens;     // x: chromatic aberration, y: grain, z: frame number (moves the grain)
    vec4 vignette; // x: strength, y: radius, z: softness, w: tonemap (0 off, 1 ACES-fitted)
};

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 lookup(vec3 c) {
    vec2 size = vec2(textureSize(u_lut, 0));
    float n = size.y;
    c = clamp(c, 0.0, 1.0) * (n - 1.0);
    float slice = floor(c.b);
    float t = c.b - slice;
    vec2 uv = vec2(c.r + 0.5, c.g + 0.5) / vec2(n * n, n);
    vec3 a = textureLod(u_lut, uv + vec2(slice / n, 0.0), 0.0).rgb;
    vec3 b = textureLod(u_lut, uv + vec2(min(slice + 1.0, n - 1.0) / n, 0.0), 0.0).rgb;
    return mix(a, b, t);
}

float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

void main() {
    vec3 color;
    if (lens.x > 0.0) {
        // Red and blue pulled apart along the line from the center, more toward the edges.
        vec2 shift = (v_uv - 0.5) * lens.x;
        color = vec3(texture(u_scene, v_uv + shift).r, texture(u_scene, v_uv).g, texture(u_scene, v_uv - shift).b);
    } else {
        color = texture(u_scene, v_uv).rgb;
    }
    color += texture(u_bloom, v_uv).rgb * grade.w;

    color *= grade.x * tint.rgb;
    if (vignette.w > 0.5) color = aces(color);

    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, grade.z);
    color = (color - 0.5) * grade.y + 0.5;

    if (tint.a > 0.0) color = mix(color, lookup(color), tint.a);

    if (vignette.x > 0.0) {
        float d = distance(v_uv, vec2(0.5)) * 1.41421356;
        color *= 1.0 - vignette.x * smoothstep(vignette.y, vignette.y + vignette.z, d);
    }
    if (lens.y > 0.0) {
        // Grain is strongest in the midtones, like film.
        float noise = hash(gl_FragCoord.xy + lens.z * vec2(17.0, 31.0)) - 0.5;
        float l = clamp(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        color += noise * lens.y * 4.0 * l * (1.0 - l);
    }
    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
