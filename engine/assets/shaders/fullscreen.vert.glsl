#version 450
// One triangle that covers the screen; draw it with 3 vertices and no buffers. v_uv is (0,0) at the top-left.

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0) (2,0) (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(p.x, 1.0 - p.y);
}
