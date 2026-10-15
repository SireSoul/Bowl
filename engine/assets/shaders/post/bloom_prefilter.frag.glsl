#version 450
// Bloom, step 1: keep only what's bright enough to glow (soft knee), at half resolution.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 texel_threshold; // xy: source texel size, z: threshold, w: knee
};

void main() {
    vec2 t = texel_threshold.xy;
    // 4-tap box (the bilinear sampler makes it a 16-texel average) to avoid fireflies.
    vec3 c = (texture(u_scene, v_uv + t * vec2(-1, -1)).rgb + texture(u_scene, v_uv + t * vec2(1, -1)).rgb +
              texture(u_scene, v_uv + t * vec2(-1, 1)).rgb + texture(u_scene, v_uv + t * vec2(1, 1)).rgb) * 0.25;
    float threshold = texel_threshold.z, knee = max(texel_threshold.w, 1e-4);
    float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4);
    out_color = vec4(c * contribution, 1.0);
}
