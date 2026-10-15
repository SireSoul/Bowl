#version 450
// Lights with normal mapping and shadows (gfx::light_material). Draw lights as usual, a soft falloff texture on a
// quad centered on the light; this shader works out where the center is from the quad's own texture coordinates, so
// lights can be any size and rotation.
//   binding 1: normals (Renderer::submit_normals), flat when nothing was drawn
//   binding 2: occluders (Renderer::submit_occluders), transparent when nothing was drawn
// The light map is half the screen's resolution; the other two are full size.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_falloff;
layout(set = 2, binding = 1) uniform sampler2D u_normals;
layout(set = 2, binding = 2) uniform sampler2D u_occluders;
layout(std140, set = 3, binding = 0) uniform Look {
    vec4 params; // x: height, y: normals on, z: shadows on
};

const int steps = 32;

void main() {
    vec4 light = texture(u_falloff, v_uv) * v_color;

    // Screen offset (light-map pixels) from this pixel to the light's center, through the inverse of how the texture
    // coordinates change across the screen.
    mat2 uv_per_pixel = mat2(dFdx(v_uv), dFdy(v_uv));
    mat2 pixel_per_uv = inverse(uv_per_pixel);
    vec2 to_light = pixel_per_uv * (vec2(0.5) - v_uv);
    float radius = max(length(pixel_per_uv * vec2(0.5, 0.0)), 1.0);
    vec2 screen = vec2(textureSize(u_occluders, 0)) * 0.5; // the light map's size, near enough
    vec2 here = gl_FragCoord.xy / screen;

    float shade = 1.0;
    if (params.y > 0.5) {
        vec3 n = texture(u_normals, here).rgb * 2.0 - 1.0;
        n.y = -n.y; // normal maps point green up; the screen's y points down
        vec3 l = normalize(vec3(to_light / radius, params.x));
        // Relative to flat ground, so unbumped sprites look exactly as without normal mapping.
        shade = min(max(dot(normalize(n), l), 0.0) / max(l.z, 0.05), 2.0);
    }

    // Occluder alpha above one half blocks light. Anything drawn there at all (even faintly, which blocks nothing) is
    // never shadowed itself: the occluders' own pixels, and upright things marked that way.
    float visible = 1.0;
    if (params.z > 0.5 && texture(u_occluders, here).a < 0.1) {
        // A different start for each pixel turns the steps' banding into fine noise.
        float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        for (int i = 0; i < steps; ++i) {
            vec2 p = (gl_FragCoord.xy + to_light * ((float(i) + jitter) / float(steps))) / screen;
            visible *= 1.0 - smoothstep(0.4, 0.6, texture(u_occluders, p).a);
            if (visible < 0.01) break;
        }
    }
    out_color = light * shade * visible;
}
