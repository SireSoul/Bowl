#version 450
// The lit scene: the scene times (ambient + light map), with emissive sprites laid over it. Runs before bloom, so what
// glows is what ends up bright after lighting.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(set = 2, binding = 1) uniform sampler2D u_light;
layout(set = 2, binding = 2) uniform sampler2D u_emissive; // premultiplied

layout(std140, set = 3, binding = 0) uniform Settings {
    vec4 ambient; // rgb: light where no light reaches, a: 1 if lighting is on
};

void main() {
    vec4 scene = texture(u_scene, v_uv);
    vec3 color = scene.rgb;
    if (ambient.a > 0.5) color *= ambient.rgb + texture(u_light, v_uv).rgb;
    vec4 emissive = texture(u_emissive, v_uv);
    color = color * (1.0 - emissive.a) + emissive.rgb;
    out_color = vec4(color, scene.a);
}
