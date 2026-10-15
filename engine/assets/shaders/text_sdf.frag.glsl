#version 450
// Distance-field text: the glyph page's alpha is the distance to the outline (0.5 on it, more inside). Sharp at any
// size; an outline is a second, wider threshold. Used through gfx::sdf_text_material().

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color; // premultiplied
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(std140, set = 3, binding = 0) uniform Look {
    vec4 outline_color; // premultiplied
    vec4 params;        // x: outline width in distance units, y: extra softness
};

void main() {
    float d = texture(u_texture, v_uv).a;
    float w = max(fwidth(d), 1e-4) * 0.75 + params.y;
    float fill = smoothstep(0.5 - w, 0.5 + w, d);
    if (params.x <= 0.0) {
        out_color = v_color * fill;
        return;
    }
    float edge = smoothstep(0.5 - params.x - w, 0.5 - params.x + w, d);
    out_color = mix(outline_color * edge, v_color, fill);
}
