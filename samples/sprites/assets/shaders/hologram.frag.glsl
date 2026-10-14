#version 450
// A custom material: shifting colors and scanlines. Edit this file while the sample runs to see it update.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color; // premultiplied
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 time; // x: seconds
};

void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float scan = 0.6 + 0.4 * sin(gl_FragCoord.y * 1.2 - time.x * 10.0);
    vec3 tint = mix(vec3(0.3, 1.0, 0.9), vec3(1.0, 0.35, 0.85), 0.5 + 0.5 * sin(time.x * 2.0 + v_uv.x * 6.0));
    out_color = vec4(c.rgb * tint * scan, c.a);
}
