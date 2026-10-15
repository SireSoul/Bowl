# Command line

## Flags every game understands

The engine reads these before the game starts. All arguments, including these, are also available to the game as
`engine.args()` and `engine.has_arg("--flag")`.

| Flag | |
| --- | --- |
| `--frames N` | Quit after N frames |
| `--screenshot <file.png>` | Save the frame shown at frame 60, or the last frame with `--frames`. Includes the overlay if it's open |
| `--overlay` | Open the dev overlay at startup (dev builds) |
| `--inspect N` | Open the overlay with the entity at index N selected |
| `--no-vsync` | Don't wait for the display's refresh; useful for measuring frame times |
| `--driver <name>` | GPU backend: `vulkan`, `metal` or `direct3d12` |
| `--lang <code>` | Language for localized strings, such as `es` |
| `--no-auto-build` | Hot-reload builds: don't rebuild the game code when a source file is saved |

Together, `--frames` and `--screenshot` make visual checks scriptable. CI uses them to render a sample on Direct3D 12:

```bash
isometric --driver direct3d12 --night --frames 30 --screenshot d3d12.png
```

## Sample flags

| Sample | Flag | |
| --- | --- | --- |
| `tutorial` | `--play` | Skip the title screen |
| `isometric` | `--night` | Start at night: lights, bloom, fireflies |
| `isometric` | `--menu` | Start with the pause menu open |
| `isometric` | `--dialogue` | Start a conversation |
| `isometric` | `--behind-pillar` | Start behind a pillar, to show y-sorting |

Games add their own the same way: `if (engine.has_arg("--skip-intro")) ...`.

## ph-shaderc

```bash
ph-shaderc <input.(vert|frag|comp).glsl> -o <output.phsh> [--depfile <file>] [-g] [--print-msl] [--print-hlsl]
           [--include <prefix>=<folder>]
```

| Option | |
| --- | --- |
| `-o <file>` | Write the `.phsh` bundle |
| `--depfile <file>` | Write a Makefile-style list of the files the shader includes |
| `-g` | Keep debug information in the SPIR-V |
| `--print-msl` | Print the generated Metal source to stdout |
| `--print-hlsl` | Print the generated HLSL to stdout |
| `--include <prefix>=<folder>` | Look for includes that start with `prefix` in `folder`; repeatable. `ph_add_game` passes `engine/shaders/=<engine shader folder>` |
| `-h`, `--help` | Usage |

Exit codes: 0 on success, 1 for compile or file errors, 2 for bad arguments. The executable is in
`build/<preset>/tools/shaderc/`. See [shaders.md](../guides/shaders.md#ph-shaderc).

## ph-atlas

```bash
ph-atlas <folder or images...> -o <atlas.png> [--padding N] [--max N] [--no-trim] [--fps N]
```

Packs PNGs into one atlas image and writes sprite-sheet JSON with the same name next to it. See
[sprite-animation.md](../guides/sprite-animation.md#packing-pictures-into-an-atlas-ph-atlas).

| Option | Default | |
| --- | --- | --- |
| `-o <file>` | | The atlas PNG; the JSON goes next to it |
| `--padding N` | 1 | Transparent pixels between pictures |
| `--max N` | 4096 | Largest width or height |
| `--no-trim` | | Keep transparent edges |
| `--fps N` | 10 | Speed of animations made from numbered files |

## Bowl Editor

See [editor.md](../guides/editor.md#starting-it).

## Test runner

`ph_tests` accepts [doctest's options](https://github.com/doctest/doctest/blob/master/doc/markdown/commandline.md).
The useful ones:

| | |
| --- | --- |
| `-tc="<pattern>"` | Run test cases whose names match, such as `-tc="ecs*"` |
| `-ltc` | List test cases |
| `-s` | Show successful checks too |
| `-d` | Print each test case's duration |
