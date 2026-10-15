#version 450
// A final-stage post effect: CRT-style scanlines over the finished image (before the UI).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_image;
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 params; // x: strength, y: lines per screen height
};

void main() {
    vec3 color = texture(u_image, v_uv).rgb;
    float line = 0.5 + 0.5 * sin(v_uv.y * params.y * 6.2831853);
    out_color = vec4(color * (1.0 - params.x * line), 1.0);
}
