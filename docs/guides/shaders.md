# Shaders

Shaders are written once, in GLSL, and run on Vulkan, Metal and Direct3D 12. The build compiles them ahead of time;
dev builds also compile them while the game runs, so saving a `.glsl` file changes what's on screen within a
fraction of a second.

- [Files and names](#files-and-names)
- [Resource bindings](#resource-bindings)
- [Writing a sprite material](#writing-a-sprite-material)
- [Includes](#includes)
- [Live editing](#live-editing)
- [ph-shaderc](#ph-shaderc)
- [The .phsh bundle](#the-phsh-bundle)
- [Engine shaders](#engine-shaders)
- [Limitations](#limitations)

## Files and names

A shader is one stage in one file, named by stage:

```
assets/shaders/hologram.frag.glsl     a fragment shader
assets/shaders/wave.vert.glsl         a vertex shader
```

Load it by its path *without* `.glsl`:

```cpp
Asset<gfx::Shader> fs = engine.assets().load<gfx::Shader>("shaders/hologram.frag");
```

Release builds load the precompiled `shaders/hologram.frag.phsh`; dev builds compile the `.glsl` source directly.
Either way, your code is the same.

The dialect is Vulkan-style GLSL: `#version 450`, explicit `layout(set = ..., binding = ...)` on every resource, and
`layout(location = ...)` on every input and output.

## Resource bindings

SDL_GPU, which the renderer is built on, fixes which descriptor set each kind of resource lives in:

| Stage | Sampled textures, storage textures, storage buffers | Uniform buffers |
| --- | --- | --- |
| Vertex | `set = 0` | `set = 1` |
| Fragment | `set = 2` | `set = 3` |

Within a set, number bindings from 0 with no gaps. Uniform blocks use `std140`; storage buffers `std430`.

If a resource is in the wrong set, `ph-shaderc` stops with an error naming the set it should be in.

## Writing a sprite material

A material is a fragment shader that replaces the sprite shader for one batch (see
[rendering.md](rendering.md#materials)). It receives:

| | Binding |
| --- | --- |
| `in vec2 v_uv` | `location = 0` |
| `in vec4 v_color` (premultiplied) | `location = 1` |
| sprite texture | `set = 2, binding = 0` |
| material textures 0 to 2 | `set = 2, binding = 1..3` |
| material uniforms (up to 256 bytes) | `set = 3, binding = 0` |

and writes premultiplied color to `location = 0`. A dissolve effect, for example:

```glsl
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_noise;     // material.textures[0]
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 dissolve; // x: amount 0..1, yzw: edge color
};

void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float n = texture(u_noise, v_uv).r;
    if (n < dissolve.x) discard;
    float edge = smoothstep(dissolve.x, dissolve.x + 0.05, n);
    out_color = vec4(mix(dissolve.yzw * c.a * 4.0, c.rgb, edge), c.a);
}
```

```cpp
struct DissolveParams {
    f32 amount;
    f32 edge[3]; // 16 bytes in total, as std140 needs
};
material.fragment = engine.assets().load<gfx::Shader>("shaders/dissolve.frag");
material.textures[0] = noise_texture.get();
material.set_uniforms(DissolveParams{0.3f, {4, 1.5f, 0.3f}});
```

The C++ struct must match the std140 layout: a `vec4` is 16 bytes, a `float` followed by a `vec3` is *not* packed
into 16 bytes (the `vec3` starts at offset 16), and the whole block is padded to a multiple of 16. Using `vec4`s
everywhere, as above, avoids surprises.

Colors in the pipeline are premultiplied: multiply RGB by alpha whenever you build a color from scratch.

## Includes

`#include "file.glsl"` works, resolved relative to the including file, then to the folder of the shader being
compiled. Included files are tracked, so editing one recompiles every shader that uses it, both in the build and in a
running dev game.

The engine's own helpers are in `engine/assets/shaders/common.glsl` (`premultiply`, `rotate`); copy what you need,
since game shaders can't include engine files by path.

## Live editing

In a dev build, edit and save any `.glsl` file under your assets (or the engine's) while the game runs. The asset
system notices within a quarter of a second, recompiles, and swaps the shader in. Pipelines using it are rebuilt on
their next use.

A shader that fails to compile logs the compiler's error, with file and line, and the last working version stays in
use. Fix the file and save again.

## ph-shaderc

The compiler is a library (`ph_shaderc`, used by dev builds at runtime) and a command-line tool:

```bash
ph-shaderc shaders/hologram.frag.glsl -o hologram.frag.phsh
```

| Option | |
| --- | --- |
| `-o <file>` | Output bundle |
| `--depfile <file>` | Write a Makefile-style dependency file listing every include (the build uses this) |
| `-g` | Keep debug information in the SPIR-V |
| `--print-msl` | Print the generated Metal source |
| `--print-hlsl` | Print the generated HLSL |

The stage comes from the file name (`.vert.glsl` or `.frag.glsl`). The pipeline inside is glslang (GLSL to SPIR-V)
and SPIRV-Cross (SPIR-V to MSL and HLSL), with resource slots remapped to what SDL_GPU expects on each backend.
`--print-msl` and `--print-hlsl` are the quickest way to see what a backend will actually run.

You rarely run it by hand: `ph_add_game(... ASSETS dir)` compiles every `*.vert.glsl` and `*.frag.glsl` under the
asset folder at build time. When cross-compiling (iOS, Android), point `PH_SHADERC_EXECUTABLE` at a host build of the
tool.

## The .phsh bundle

A `.phsh` file holds, for one shader stage:

- SPIR-V, for Vulkan
- Metal Shading Language source, for Metal
- HLSL source (Shader Model 5.1), for Direct3D 12, compiled to DXBC when the game loads it
- the resource counts SDL_GPU needs to create the shader (samplers, storage textures, storage buffers, uniform
  buffers)

The device picks the right blob for its backend. The format is versioned; a bundle from an older build is rejected
with a message to rebuild shaders.

## Engine shaders

| File | Used for |
| --- | --- |
| `sprite.vert.glsl` | Builds each sprite's quad from the sprite storage buffer (vertex pulling) |
| `sprite.frag.glsl` | Texture times color |
| `fullscreen.vert.glsl` | A full-screen triangle for post-processing passes |
| `post/bloom_prefilter.frag.glsl` | Bloom's bright pass |
| `post/bloom_down.frag.glsl`, `post/bloom_up.frag.glsl` | The bloom blur chain |
| `post/final.frag.glsl` | Lighting, bloom, grading, tonemapping, vignette |

They live in `engine/assets/shaders/` and hot-reload like game shaders, which is handy when working on the renderer.

## Limitations

- Vertex and fragment shaders only. Compute shaders aren't supported by `ph-shaderc` yet.
- Materials replace the fragment stage only; sprites always use the engine's vertex shader.
- Direct3D 12 compiles shaders at load time (fast, but not free); caching the result on disk is on the roadmap.
