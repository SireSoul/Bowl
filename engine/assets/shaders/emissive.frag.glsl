#version 450
// A material's glow (gfx::SpriteMaterial): its emissive map in the glow's color, see-through where the map is dark so
// black glows nowhere. Drawn into the emissive layer, which lies over the lit scene.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color; // premultiplied: the glow's color times its strength
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;

void main() {
    vec4 t = texture(u_texture, v_uv); // premultiplied
    float glow = max(t.r, max(t.g, t.b));
    out_color = vec4(t.rgb * v_color.rgb, glow * v_color.a);
}
