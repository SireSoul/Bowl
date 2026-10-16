# Getting started

This page takes you from a fresh machine to running the samples and tests. It takes about ten minutes, most of it
the first build, which compiles SDL3 (unless it's installed), glslang and SPIRV-Cross from source.

- [Requirements](#requirements)
- [Windows](#windows)
- [macOS](#macos)
- [Linux](#linux)
- [Build](#build)
- [Presets](#presets)
- [Run the samples](#run-the-samples)
- [Run the tests](#run-the-tests)
- [Editors and IDEs](#editors-and-ides)
- [Next steps](#next-steps)

## Requirements

| | Minimum |
| --- | --- |
| CMake | 3.25 (3.27 for game-code hot reload) |
| Compiler | Visual Studio 2022, Xcode 15 (Apple Clang), Clang 16 or GCC 13 |
| GPU | Direct3D 12, Metal or Vulkan, as supported by [SDL_GPU](https://wiki.libsdl.org/SDL3/CategoryGPU) |
| Git | any recent version |

The engine needs C++20, including `<format>`, which is why the compiler minimums are what they are.

Every library the engine uses is downloaded by CMake at a pinned version and checked against a SHA-256 hash (see
`cmake/BowlDependencies.cmake`), so there's nothing else to install by hand. If SDL3 is already installed, the build
uses it; otherwise it builds SDL3 from source.

## Windows

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is fine) with the **Desktop
   development with C++** workload. It includes CMake and Git.
2. Open **Developer PowerShell for VS 2022** so `cmake` and the compiler are on the path. Plain PowerShell works too
   if CMake and Git are installed separately.

The presets don't name a generator, so on Windows CMake uses the Visual Studio generator. Executables end up in a
configuration subfolder, for example `build\dev\samples\isometric\RelWithDebInfo\isometric.exe`.

The Direct3D 12 backend compiles shaders at load time with `d3dcompiler_47.dll`, which ships with Windows 10 and 11.

## macOS

1. Install Xcode, or just its command line tools:

   ```bash
   xcode-select --install
   ```

2. Install CMake, for example with [Homebrew](https://brew.sh):

   ```bash
   brew install cmake
   ```

3. Optional, for a faster first build: install SDL3 so it isn't compiled from source.

   ```bash
   brew install sdl3
   ```

## Linux

Install a compiler, CMake, Ninja and the headers SDL3 needs for windows, input and audio. On Ubuntu 24.04 or Debian
13:

```bash
sudo apt install build-essential cmake ninja-build git libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libwayland-dev libxkbcommon-dev wayland-protocols libegl1-mesa-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev
```

This is the same list CI uses (`.github/workflows/ci.yml`). You also need a Vulkan driver: Mesa's (`mesa-vulkan-drivers`)
for AMD and Intel, or NVIDIA's proprietary driver.

On Fedora, the equivalents are `gcc-c++ cmake ninja-build libX11-devel libXext-devel libXrandr-devel libXcursor-devel
libXi-devel libXScrnSaver-devel libXfixes-devel libXtst-devel wayland-devel libxkbcommon-devel wayland-protocols-devel
mesa-libEGL-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel dbus-devel systemd-devel`.

## Build

```bash
git clone https://github.com/SireSoul/Bowl.git
```

```bash
cd Bowl
```

```bash
cmake --preset dev
```

```bash
cmake --build --preset dev
```

The build folder is `build/dev`. Later builds only recompile what changed. To build a single target, add
`--target <name>`, for example `--target isometric`.

## Presets

`CMakePresets.json` defines three configurations:

| Preset | Build type | Dev tools | Use it for |
| --- | --- | --- | --- |
| `dev` | RelWithDebInfo | on | Day-to-day work: optimized, debuggable, with the overlay, hot reload and the runtime shader compiler |
| `debug` | Debug | on | Stepping through code. Hot engine code (ECS, math) is still built with light optimization so games stay playable |
| `release` | Release | off | Shipping: no dev tools, precompiled shaders only, SDL linked statically, macOS `.app` bundles |

Each preset has a build preset with the same name, and `dev` and `release` have test presets. You can override any
option on the configure line, for example `cmake --preset dev -DBOWL_FETCH_SDL=ON`. All options are listed in
[reference/cmake.md](reference/cmake.md).

## Run the samples

Each sample is an executable in `build/<preset>/samples/<name>/` (plus `RelWithDebInfo/` or `Release/` on Windows).
They can be started from any working directory.

```bash
./build/dev/samples/tutorial/tutorial
```

```bash
./build/dev/samples/isometric/isometric --night
```

```bash
./build/dev/samples/sprites/sprites
```

Press **F1** in any of them for the dev overlay. Flags every game understands, such as `--screenshot` and
`--driver`, are listed in [reference/command-line.md](reference/command-line.md).

## Run the tests

```bash
ctest --preset dev
```

`ctest` runs one executable, `bowl_tests`, which holds every test case. Running it directly gives more control, using
[doctest's options](https://github.com/doctest/doctest/blob/master/doc/markdown/commandline.md):

```bash
./build/dev/tests/bowl_tests -tc="ecs*"
```

There's also an ECS benchmark that prints timings. It isn't a test; run it from a `release` build for meaningful
numbers:

```bash
./build/release/tests/bowl_bench_ecs
```

## Editors and IDEs

Every configure writes `compile_commands.json` into the build folder, so clangd-based editors get completion and
diagnostics.

- **Visual Studio 2022:** File > Open > Folder, then pick the `dev` configuration in the toolbar. Visual Studio reads
  `CMakePresets.json` directly.
- **VS Code:** install the CMake Tools extension (and clangd or the C/C++ extension). It picks up the presets; choose
  `dev` when asked.
- **CLion:** open the folder; enable the `dev` preset under Settings > Build, Execution, Deployment > CMake.
- **Xcode:** generate a project into its own folder, then open the `.xcodeproj` it writes:

  ```bash
  cmake --preset dev -G Xcode -B build/xcode
  ```

When debugging a game, its working directory doesn't matter: assets are found relative to the executable, and dev
builds read them straight from the source folders.

## Next steps

- Make a game: [tutorial.md](tutorial.md)
- Start your own project outside this repository: [reference/cmake.md](reference/cmake.md#using-the-engine-in-your-own-project)
- Learn how the engine is put together: [architecture.md](architecture.md)
- Something went wrong: [troubleshooting.md](troubleshooting.md)
