#version 450
// Bloom, step 3: upsample with a 3x3 tent filter and add onto the next larger mip (additive blending).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_source;
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 texel_radius; // xy: source texel size, z: radius in texels
};

void main() {
    vec2 t = texel_radius.xy * texel_radius.z;
    vec3 sum = texture(u_source, v_uv).rgb * 4.0;
    sum += (texture(u_source, v_uv + t * vec2(0, -1)).rgb + texture(u_source, v_uv + t * vec2(-1, 0)).rgb +
            texture(u_source, v_uv + t * vec2(1, 0)).rgb + texture(u_source, v_uv + t * vec2(0, 1)).rgb) * 2.0;
    sum += texture(u_source, v_uv + t * vec2(-1, -1)).rgb + texture(u_source, v_uv + t * vec2(1, -1)).rgb +
           texture(u_source, v_uv + t * vec2(-1, 1)).rgb + texture(u_source, v_uv + t * vec2(1, 1)).rgb;
    out_color = vec4(sum / 16.0, 1.0);
}
