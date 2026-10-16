# CMake reference

- [Options](#options)
- [Presets](#presets)
- [Targets](#targets)
- [ph_add_game](#ph_add_game)
- [ph_package](#ph_package)
- [ph_compile_shaders](#ph_compile_shaders)
- [Other helpers](#other-helpers)
- [Using the engine in your own project](#using-the-engine-in-your-own-project)
- [Dependencies](#dependencies)

## Options

Set them on the configure line (`-DPH_DEV_TOOLS=OFF`) or in your own presets.

| Option | Default | |
| --- | --- | --- |
| `PH_DEV_TOOLS` | `ON` | The F1 overlay, the runtime shader compiler, asset hot reload, game-code hot reload, assertions. Turn off for shipping |
| `PH_BUILD_TESTS` | `ON` when the engine is the top-level project | Unit tests and the ECS benchmark |
| `PH_BUILD_SAMPLES` | `ON` when the engine is the top-level project | `tutorial`, `isometric`, `sprites` |
| `PH_FETCH_SDL` | `OFF` | Build SDL3 from source (statically) even if an installed SDL3 is found |
| `PH_WARNINGS_AS_ERRORS` | `OFF` | Treat warnings in engine, tool, test and game code as errors (CI turns this on for Linux and macOS) |
| `PH_MACOS_BUNDLES` | `OFF` | Build macOS games as `.app` bundles |
| `PH_PACK_ASSETS` | `OFF` | Pack each game's `engine/` and `assets/` folders into `.phpak` archives ([assets](../guides/assets.md#archives)) |
| `PH_MACOS_SIGN_IDENTITY` | `-` | codesign identity for installed `.app` bundles: `-` signs ad hoc, `Developer ID Application: ...` for shipping, empty skips ([packaging](../guides/packaging.md#signing-and-notarization)) |
| `PH_MACOS_ENTITLEMENTS` | empty | An entitlements file for codesign |
| `PH_WINDOWS_SIGN_COMMAND` | empty | A command (a CMake list) run on each installed `.exe` to sign it, such as `signtool;sign;/fd;SHA256;/a` |
| `PH_LINUX_SHARE` | `OFF` | Also install the desktop file and icon under `share/` and the game under `bin/` (Flatpak, distro packages) |
| `PH_BUILD_EDITOR` | `ON` when the engine is the top-level project | [Bowl Editor](../guides/editor.md) (needs `PH_DEV_TOOLS`) |
| `PH_TRACY` | `OFF` | Send profiler zones, frames, plots and log lines to the [Tracy](../guides/jobs-and-profiling.md#tracy) profiler |
| `PH_BOX2D` | `OFF` | Rigid-body physics through Box2D v3 ([collision.md](../guides/collision.md)) |
| `PH_OPUS` | `OFF` | Ogg Opus sounds and music; fetches libogg, libopus and opusfile ([audio.md](../guides/audio.md#opus)) |
| `PH_HARFBUZZ` | `OFF` | Shape text with HarfBuzz: joined Arabic, Indic scripts, ligatures ([text.md](../guides/text.md#right-to-left-text-and-shaping)) |
| `PH_HOST_TOOLS` | empty | Cross-compiling: a folder with `ph-shaderc`, `ph-icon` and `ph-pack` built for this machine (the `host-tools` preset makes `build/host-tools/bin`); the build uses them instead of building its own |
| `PH_SHADERC_EXECUTABLE` | empty | A prebuilt host `ph-shaderc` for cross-compiling; when set, the shader compiler isn't built (`PH_HOST_TOOLS` sets it) |
| `PH_IOS_TEAM` | empty | iOS: the Apple development team that signs apps for devices; empty signs ad hoc, for the simulator |
| `PH_ECS_MAX_COMPONENTS` | 256 | (A preprocessor define, not a CMake option.) Maximum component types; a multiple of 64 |

## Presets

From `CMakePresets.json`. Each configure preset has a matching build preset; `dev` and `release` have test presets.

| Preset | `CMAKE_BUILD_TYPE` | Sets |
| --- | --- | --- |
| `dev` | `RelWithDebInfo` | `PH_DEV_TOOLS=ON` |
| `debug` | `Debug` | `PH_DEV_TOOLS=ON` |
| `release` | `Release` | `PH_DEV_TOOLS=OFF`, `PH_FETCH_SDL=ON`, `PH_MACOS_BUNDLES=ON`, `PH_PACK_ASSETS=ON` |
| `host-tools` | `Release` | Only the tools cross builds run; its build preset makes `ph-shaderc`, `ph-icon` and `ph-pack` |
| `ios` | (Xcode) | The Xcode generator, `CMAKE_SYSTEM_NAME=iOS`, iOS 17, `PH_PACK_ASSETS=ON`, `PH_HOST_TOOLS=build/host-tools/bin`. Build presets `ios-simulator` and `ios-device` |

All presets build into `build/<preset>` and export `compile_commands.json`. The tools a build makes (`ph-shaderc`,
`ph-icon`, `ph-pack`, `ph-atlas`, `ph-apidoc`) go in its `bin` folder.

## Targets

| Target | |
| --- | --- |
| `ph::engine` | Everything; what games link |
| `ph::core`, `ph::ecs`, `ph::platform`, `ph::gfx`, `ph::audio`, `ph::physics`, `ph::ui`, `ph::net`, `ph::app` | The individual modules, for tools that need only part of the engine |
| `ph-shaderc` | The shader compiler executable |
| `ph_shaderc` | The shader compiler as a library |
| `ph-icon` | Makes application icons (`.icns`, `.ico`, PNGs) from one picture; `ph_add_game(... ICON)` runs it |
| `ph-pack` | Packs a folder into an asset archive (`.phpak`), or lists one |
| `ph-apidoc`, `ph_api_docs` | Writes the [API reference](api/README.md) from the headers; `ph_api_docs` runs it into `docs/reference/api` |
| `ph_engine_assets` | Compiles the engine's shaders |
| `ph_tests`, `ph_bench_ecs` | Tests and benchmark |
| `<game>_assets` | Created by `ph_add_game`: copies and compiles a game's assets |
| `<game>_code` | Created by `ph_add_game(... HOT_RELOAD)`: the game's code as a loadable library |
| `<game>_appimage` | Created by `ph_package` on Linux: an AppImage of the game |
| `bowl-editor` | The [editor](../guides/editor.md) |

## ph_add_game

```cmake
ph_add_game(<target> [ASSETS <dir>] [NAME <name>] [ICON <png>] [ICON_PIXEL_ART] [BUNDLE_ID <id>] [HOT_RELOAD])
```

Turns an executable target into a game:

- links `ph::engine` and applies the engine's compiler settings (C++20, warnings);
- compiles every `*.vert.glsl` and `*.frag.glsl` under `ASSETS` into `.phsh` bundles;
- copies the engine's assets and the game's assets next to the executable, as `engine/` and `assets/`;
- in dev builds, tells the game where its source asset folder is, so it reads (and hot-reloads) assets from there;
- with an `ICON`, makes the platform's icon files and puts the icon in the executable for the window;
- adds install rules, signing the installed copy (see [packaging](../guides/packaging.md)).

| Argument | |
| --- | --- |
| `ASSETS <dir>` | The game's asset folder, relative to the current `CMakeLists.txt`. Optional for games with no files |
| `NAME <name>` | The name players see: the macOS bundle name, Linux menus, installers. Default `<target>` |
| `ICON <png>` | The application icon: a square PNG, 1024 x 1024 at best. Becomes the macOS `.icns`, the Windows `.ico` (in the `.exe`), the Linux PNG, and the window's icon |
| `ICON_PIXEL_ART` | Scale the icon with hard pixel edges, for small pixel-art icons |
| `BUNDLE_ID <id>` | macOS and iOS bundle identifier, Flatpak app id. Default `com.bowl.<target>` |
| `ORIENTATION <which>` | iOS: `landscape` (the default), `portrait` or `any` |
| `HOT_RELOAD` | Build the game's sources as `<target>_code`, a library the `<target>` host reloads when it's rebuilt (and rebuilds it when a source file is saved). Dev builds with CMake 3.27+; ignored elsewhere. See [hot-reload.md](../guides/hot-reload.md#game-code) |

Add sources to the target before calling `ph_add_game` when using `HOT_RELOAD`: the sources are moved into the library
at that point.

## ph_package

```cmake
ph_package(<target> [VENDOR <name>] [CONTACT <email>] [DESCRIPTION <text>] [LICENSE <file>])
```

Installers for one game, made by running `cpack` in the build folder after building: a `.dmg` and a `.zip` on
macOS, an NSIS installer (when `makensis` is installed) and a `.zip` on Windows, a `.tar.gz` on Linux, each with a
SHA-256 file. On Linux it also adds `<target>_appimage` and writes `<bundle id>.flatpak.json`. The name, version, icon
and bundle id come from `ph_add_game` and `project()`. Call it once per build. See
[packaging](../guides/packaging.md#installers).

## ph_compile_shaders

```cmake
ph_compile_shaders(<outputs_var> <source_dir> <output_dir>)
```

Adds a build rule for every shader under `source_dir` that writes `output_dir/<relative path>.phsh`, with dependency
tracking for `#include`s, and stores the list of outputs in `outputs_var`. `ph_add_game` uses it; call it directly for
shaders that live outside a game's asset folder, then depend on the outputs:

```cmake
ph_compile_shaders(tool_shaders ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_BINARY_DIR}/shaders)
add_custom_target(tool_shaders ALL DEPENDS ${tool_shaders})
```

## Other helpers

| Function | |
| --- | --- |
| `ph_configure_target(<target>)` | The engine's compiler settings: C++20, warnings (`/W4`, or `-Wall -Wextra -Wpedantic -Wshadow ...`), `PH_DEV_TOOLS`, Windows defines. `ph_add_game` calls it |
| `ph_optimize_debug(<target>)` | Light optimization in Debug builds (`-Og`, `/Ob1`), for code that's too slow unoptimized |

## Using the engine in your own project

Games don't have to live in this repository. Two ways to bring the engine in:

**As a subdirectory** (a Git submodule, or a checkout next to your game):

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_game VERSION 0.1.0 LANGUAGES C CXX)

add_subdirectory(external/Bowl)

add_executable(my_game src/main.cpp)
ph_add_game(my_game ASSETS assets)
```

If the engine's folder isn't inside your project's folder, pass a build directory name as the second argument:
`add_subdirectory(../Bowl engine)`.

**With FetchContent**, pinned to a commit or tag:

```cmake
include(FetchContent)
FetchContent_Declare(bowl
    GIT_REPOSITORY https://github.com/SireSoul/BOWL.git
    GIT_TAG <commit or tag>)
FetchContent_MakeAvailable(bowl)

add_executable(my_game src/main.cpp)
ph_add_game(my_game ASSETS assets)
```

While the repository is private, FetchContent needs Git credentials that can read it.

In both cases, the engine's tests and samples are off (they default on only when the engine is the top-level
project), and `PH_*` options can be set before `add_subdirectory` / `FetchContent_MakeAvailable` or on the command
line. Copy the engine's `CMakePresets.json` into your project for the same `dev`, `debug` and `release` setups.

## Dependencies

Fetched by `cmake/PhDependencies.cmake`, each pinned to a release archive and its SHA-256:

| Library | Version | Used for | License |
| --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | 3.4.16 | Windows, input, audio, GPU, filesystem | zlib |
| [stb](https://github.com/nothings/stb) | pinned commit | Image decoding, PNG writing, TrueType, Vorbis | MIT or public domain |
| [glslang](https://github.com/KhronosGroup/glslang) | 16.5.0 | GLSL to SPIR-V (shader compiler only) | BSD-3-Clause and others |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | vulkan-sdk-1.4.341.0 | SPIR-V to MSL and HLSL (shader compiler only) | Apache-2.0 |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | The dev overlay (dev builds only) | MIT |
| [doctest](https://github.com/doctest/doctest) | 2.5.2 | Unit tests (tests only) | MIT |
| [Tracy](https://github.com/wolfpld/tracy) | 0.12.2 | Profiler client (`PH_TRACY` only) | BSD-3-Clause |
| [Box2D](https://github.com/erincatto/box2d) | 3.1.1 | Rigid bodies (`PH_BOX2D` only) | MIT |
| [libogg](https://xiph.org/ogg/), [libopus](https://opus-codec.org), [opusfile](https://opus-codec.org) | 1.3.5, 1.5.2, 0.12 | Opus sounds (`PH_OPUS` only) | BSD-3-Clause |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 14.5.1 | Text shaping (`PH_HARFBUZZ` only) | MIT ("Old MIT") |

An installed SDL3 (3.2 or newer) is used instead of building it when `PH_FETCH_SDL` is off.

To update a dependency, change its URL and hash together. CMake prints the actual hash when it doesn't match, which is
a convenient way to get the new one.
