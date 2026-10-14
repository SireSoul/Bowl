#version 450
// Textures are premultiplied at load time; blending is ONE, ONE_MINUS_SRC_ALPHA.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

void main() {
    out_color = texture(u_texture, v_uv) * v_color;
}
