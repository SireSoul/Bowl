#version 450
// The last pass: light map, bloom, color grading and vignette, from the HDR scene to the displayed image.
// With default settings it is an exact copy, so games that don't use any of it look unchanged.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(set = 2, binding = 1) uniform sampler2D u_bloom;
layout(set = 2, binding = 2) uniform sampler2D u_light;

layout(std140, set = 3, binding = 0) uniform Settings {
    vec4 grade;    // x: exposure, y: contrast, z: saturation, w: bloom intensity
    vec4 tint;     // rgb multiplier, a unused
    vec4 ambient;  // rgb: light where no light reaches, a: 1 if the light map is on
    vec4 vignette; // x: strength, y: radius, z: softness, w: tonemap (0 off, 1 ACES-fitted)
};

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 scene = texture(u_scene, v_uv);
    vec3 color = scene.rgb;

    if (ambient.a > 0.5) color *= ambient.rgb + texture(u_light, v_uv).rgb;
    color += texture(u_bloom, v_uv).rgb * grade.w;

    color *= grade.x * tint.rgb;
    if (vignette.w > 0.5) color = aces(color);

    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, grade.z);
    color = (color - 0.5) * grade.y + 0.5;

    if (vignette.x > 0.0) {
        float d = distance(v_uv, vec2(0.5)) * 1.41421356;
        color *= 1.0 - vignette.x * smoothstep(vignette.y, vignette.y + vignette.z, d);
    }
    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
