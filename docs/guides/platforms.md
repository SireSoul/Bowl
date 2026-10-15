# Platforms

The engine runs on SDL3, which handles windows, input, audio and the GPU on each platform. Game code is the same
everywhere; no `#ifdef`s are needed for the supported platforms.

## Status

| Platform | Architectures | GPU backend | Status |
| --- | --- | --- | --- |
| Windows 10, 11 | x64 | Direct3D 12 (default), Vulkan | Built and tested on CI. Every CI run renders the isometric sample on Direct3D 12 through WARP (Microsoft's software renderer) and keeps the screenshot |
| macOS | Apple silicon, Intel | Metal | Built and tested on CI. The main development platform |
| Linux | x64 | Vulkan | Built and tested on CI (X11 and Wayland) |
| iOS, iPadOS | arm64 | Metal | Not set up yet |
| Android | arm64 | Vulkan | Not set up yet |
| Web | wasm | (needs WebGPU in SDL_GPU) | Planned |
| Consoles | | | Through SDL's console ports, which need developer agreements with each platform holder |

"Built and tested" means CI builds the `dev` and `release` presets with warnings as errors (on Linux and macOS) and
runs the full test suite, on every push.

## GPU backends

SDL_GPU chooses the best backend for the platform. Force one with `--driver` or `AppConfig::gpu_driver`:

```bash
./my_game --driver vulkan
```

| Backend | Shaders |
| --- | --- |
| Direct3D 12 | HLSL from the shader bundle, compiled to DXBC when loaded |
| Metal | MSL from the shader bundle |
| Vulkan | SPIR-V from the shader bundle |

Hardware and driver minimums are SDL_GPU's; see the
[SDL_CreateGPUDevice documentation](https://wiki.libsdl.org/SDL3/SDL_CreateGPUDevice) for each backend's.

The backend in use is printed at startup (`ready: 17 worker threads, metal backend`) and shown in the overlay.

## Why the entry point is a callback

SDL3 offers two ways to run a program: a classic `main()` with a loop, or callbacks (`SDL_AppInit`,
`SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`) that SDL calls. The web, iOS and Android need the callback style,
because the operating system owns the loop there. The engine uses callbacks on every platform, through `PH_MAIN`, so
a game written on a desktop doesn't need changes later.

## High-DPI displays

Game coordinates are in logical points. On a high-DPI display (a Retina Mac, a 4K monitor at 200% scaling), the
window has more physical pixels than points: `window().size()` is in points, `window().pixel_size()` in pixels, and
`window().pixel_density()` is the ratio. Rendering happens at full physical resolution automatically; pass
`pixel_density()` to text and UI so glyphs rasterize sharply (see [text.md](text.md)).

## Platform notes

**Windows.** The default generator is Visual Studio, a multi-config generator: executables are in a
`RelWithDebInfo` or `Release` subfolder. The editor can't load a game's code library there yet (`--game`).

**macOS.** Release builds produce `.app` bundles; dev builds produce plain executables for faster iteration. Apps
aren't signed or notarized yet, so other Macs show a Gatekeeper warning.

**Linux.** SDL builds with X11 and Wayland support and picks one at runtime. Audio goes through PipeWire,
PulseAudio or ALSA, whichever is available.

## Cross-compiling

To build for a platform that can't run the build tools (iOS, Android, consoles), build `ph-shaderc` for the host
first and point the target build at it:

```bash
cmake -B build/ios ... -DPH_SHADERC_EXECUTABLE=/path/to/host/ph-shaderc
```

The engine then doesn't need to build the shader compiler for the target.
