# Shaders

Shaders are written once, in GLSL, and run on Vulkan, Metal and Direct3D 12. The build compiles them ahead of time;
dev builds also compile them while the game runs, so saving a `.glsl` file changes what's on screen within a
fraction of a second.

- [Files and names](#files-and-names)
- [Resource bindings](#resource-bindings)
- [Writing a sprite material](#writing-a-sprite-material)
- [Parameters and what the editor shows](#parameters-and-what-the-editor-shows)
- [Includes](#includes)
- [Live editing](#live-editing)
- [bowl-shaderc](#bowl-shaderc)
- [The .bowlsh bundle](#the-bowlsh-bundle)
- [Engine shaders](#engine-shaders)
- [Limitations](#limitations)

## Files and names

A shader is one stage in one file, named by stage:

```
assets/shaders/hologram.frag.glsl     a fragment shader
assets/shaders/wave.vert.glsl         a vertex shader
assets/shaders/flock.comp.glsl        a compute shader
```

Load it by its path *without* `.glsl`:

```cpp
Asset<gfx::Shader> fs = engine.assets().load<gfx::Shader>("shaders/hologram.frag");
```

Release builds load the precompiled `shaders/hologram.frag.bowlsh`; dev builds compile the `.glsl` source directly.
Either way, your code is the same.

The dialect is Vulkan-style GLSL: `#version 450`, explicit `layout(set = ..., binding = ...)` on every resource, and
`layout(location = ...)` on every input and output.

## Resource bindings

SDL_GPU, which the renderer is built on, fixes which descriptor set each kind of resource lives in:

| Stage | Sampled textures, storage textures, storage buffers | Uniform buffers |
| --- | --- | --- |
| Vertex | `set = 0` | `set = 1` |
| Fragment | `set = 2` | `set = 3` |

Compute shaders have their own layout:

| Set | Holds |
| --- | --- |
| `set = 0` | Sampled textures, then `readonly` storage textures and buffers |
| `set = 1` | Read-write storage textures and buffers |
| `set = 2` | Uniform buffers |

Storage marked `readonly` in GLSL goes in set 0; anything the shader writes goes in set 1.

Within a set, number bindings from 0 with no gaps. Uniform blocks use `std140`; storage buffers `std430`.

If a resource is in the wrong set, `bowl-shaderc` stops with an error naming the set it should be in.

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

A [material file](rendering.md#material-files) does the same without code: it names the shader, its textures and its
parameters, by name.

## Parameters and what the editor shows

The members of a shader's uniform block at binding 0 are its parameters. The compiler records each one's place in the
block, and a comment after its declaration says more about it:

```glsl
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 glow;       // color = 1, 0.6, 0.2   The glow's color
    float speed;     // = 2 [0, 10]           How fast it scrolls
    vec2 scale;      // = 3
    int steps;       // [1, 8]
    float bowl_time; // filled in by the renderer
};
layout(set = 2, binding = 1) uniform sampler2D u_noise; // Clouds to scroll
```

| In the comment | Means |
| --- | --- |
| `color` | Shown as a color picker |
| `= a, b, c, d` | Its value when a material doesn't set it. One number fills every part; a color's missing alpha is 1. Without one it's 0 |
| `[lo, hi]` | A slider from `lo` to `hi` |
| the rest | What it's for, shown on its name |

The parts can come in any order, before the help. A texture's comment is its help.

Those defaults count everywhere: the renderer fills any part of the block that a material's `uniforms` don't cover with
them, so a `Material` with no `set_uniforms` call at all draws with the shader's defaults. Two names are filled in by
the renderer for every draw, in sprite materials and post effects alike:

| Member | Holds |
| --- | --- |
| `float bowl_time` | Seconds (`Engine::time()`, so it stops while the game is paused) |
| `vec4 bowl_time` | Seconds, the frame's step, the frame number, 0 |
| `vec2`/`vec4 bowl_screen` | The target's width and height in pixels, then one over each |

Bowl Editor's Materials workspace reads all of this: a material's form shows each parameter with its control, default
and help, and the shader's extra textures by name (see [the editor guide](editor.md#the-materials-workspace)).
`gfx::Shader::uniforms()`, `textures()` and `defaults()` give the same to your own tools.

## Includes

`#include "file.glsl"` works, resolved relative to the including file, then to the folder of the shader being
compiled. Included files are tracked, so editing one recompiles every shader that uses it, both in the build and in a
running dev game.

Game shaders can include the engine's shader files by their asset path:

```glsl
#include "engine/shaders/common.glsl"   // premultiply(), rotate()
```

`bowl_add_game` passes bowl-shaderc the engine's shader folder for that prefix, and dev builds find it wherever the
engine's assets are mounted.

## Live editing

In a dev build, edit and save any `.glsl` file under your assets (or the engine's) while the game runs. The asset
system notices within a quarter of a second, recompiles, and swaps the shader in. Pipelines using it are rebuilt on
their next use.

A shader that fails to compile logs the compiler's error, with file and line, and the last working version stays in
use. Fix the file and save again.

## bowl-shaderc

The compiler is a library (`bowl_shaderc`, used by dev builds at runtime) and a command-line tool:

```bash
bowl-shaderc shaders/hologram.frag.glsl -o hologram.frag.bowlsh
```

| Option | |
| --- | --- |
| `-o <file>` | Output bundle |
| `--depfile <file>` | Write a Makefile-style dependency file listing every include (the build uses this) |
| `-g` | Keep debug information in the SPIR-V |
| `--print-msl` | Print the generated Metal source |
| `--print-hlsl` | Print the generated HLSL |
| `--include <prefix>=<folder>` | Look for includes that start with `prefix` in `folder` (repeatable) |

The stage comes from the file name (`.vert.glsl` or `.frag.glsl`). The pipeline inside is glslang (GLSL to SPIR-V)
and SPIRV-Cross (SPIR-V to MSL and HLSL), with resource slots remapped to what SDL_GPU expects on each backend.
`--print-msl` and `--print-hlsl` are the quickest way to see what a backend will actually run.

You rarely run it by hand: `bowl_add_game(... ASSETS dir)` compiles every `*.vert.glsl`, `*.frag.glsl` and `*.comp.glsl`
under the asset folder at build time. When cross-compiling (iOS, Android), point `BOWL_SHADERC_EXECUTABLE` at a host build of the
tool.

## The .bowlsh bundle

A `.bowlsh` file holds, for one shader stage:

- SPIR-V, for Vulkan
- Metal Shading Language source, for Metal
- HLSL source (Shader Model 5.1), for Direct3D 12, compiled to DXBC when the game loads it
- the resource counts SDL_GPU needs to create the shader (samplers, storage textures, storage buffers, uniform
  buffers) and, for compute shaders, the read-write counts and the thread-group size
- the uniform block's layout (each member's name, type, place, default, range and help) and the textures' names, for
  materials (format 3)

The device picks the right blob for its backend. On Direct3D 12 the HLSL is compiled to DXBC the first time a
shader loads and kept in the player's data folder (`shader-cache/`), so later runs skip the compile; a changed shader
never matches an old entry. The format is versioned; a bundle from an older build is rejected
with a message to rebuild shaders.

## Engine shaders

| File | Used for |
| --- | --- |
| `sprite.vert.glsl` | Builds each sprite's quad from the sprite storage buffer (vertex pulling) |
| `sprite.frag.glsl` | Texture times color |
| `fullscreen.vert.glsl` | A full-screen triangle for post-processing passes |
| `post/bloom_prefilter.frag.glsl` | Bloom's bright pass |
| `post/bloom_down.frag.glsl`, `post/bloom_up.frag.glsl` | The bloom blur chain |
| `post/final.frag.glsl` | Bloom, grading, tonemapping, LUT, lens effects, vignette |
| `post/composite.frag.glsl` | Lighting: the scene times the light map, with emissive sprites over it |
| `emissive.frag.glsl` | A material's glow: its emissive map in the glow's color, see-through where the map is dark |
| `light.frag.glsl` | Lights with normal mapping and shadows (`light_material`) |
| `text_sdf.frag.glsl` | Distance-field text (`sdf_text_material`) |
| `particles.comp.glsl` | GPU particles (`GpuParticles`) |

They live in `engine/assets/shaders/` and hot-reload like game shaders, which is handy when working on the renderer.

## Compute shaders

Compute shaders run work on the GPU that isn't drawing: simulations, flocking, generating data. Load one as a
`gfx::ComputeShader`, give it `gfx::GpuBuffer`s to work on, and dispatch it through the renderer:

```glsl
#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer In { vec2 targets[]; };
layout(std430, set = 1, binding = 0) buffer Boids { vec4 boids[]; };   // xy position, zw velocity
layout(std140, set = 2, binding = 0) uniform Params { vec4 params; };    // x: dt, y: count

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(params.y)) return;
    vec4 b = boids[i];
    b.zw += (targets[i] - b.xy) * params.x;
    b.xy += b.zw * params.x;
    boids[i] = b;
}
```

```cpp
Asset<gfx::ComputeShader> flock = engine.assets().load<gfx::ComputeShader>("shaders/flock.comp");
auto boids = engine.gpu().create_buffer(count * sizeof(Vec4), "boids");      // starts zeroed
auto targets = engine.gpu().create_buffer(count * sizeof(Vec2), "targets");
targets->upload(target_data.data(), count * sizeof(Vec2));

// every frame
gfx::ComputeDispatch job;
job.shader = flock.get();
job.read = {targets.get()};
job.write = {boids.get()};
job.set_uniforms(Params{engine.delta(), f32(count), 0, 0});
job.cover(count);                     // enough groups of 64 for every boid
engine.renderer().dispatch(std::move(job));
```

Dispatches run at the start of the frame's GPU work, in the order they're made, before anything is drawn, so a
buffer a compute shader writes can be drawn in the same frame. A shader that writes `SpriteInstance`s (the layout
in `sprite.vert.glsl`) can be drawn straight from its buffer with
`renderer.submit(buffer, count, texture, camera, blend)`, without the data ever coming back to the CPU; GPU
particles work that way (see [particles](particles-and-tweens.md#gpu-particles)).

Buffers are read and written by compute shaders and read by sprite drawing. Storage textures aren't bound by
`ComputeDispatch` yet.

## Shader graphs

Bowl Editor's shader graphs (`.shadergraph` files) write a `.frag.glsl` beside themselves when saved, so a game builds
and loads them like any other shader; nothing at runtime knows they came from a graph. The generated file says so at
the top: change the graph, not the file. See [the editor guide](editor.md#shader-graphs).

## Limitations

- Materials replace the fragment stage only; sprites always use the engine's vertex shader.
- Direct3D 12 uses DXBC (Shader Model 5.1). DXIL (Shader Model 6) would need DXC.
