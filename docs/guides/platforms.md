# Platforms

The engine runs on SDL3, which handles windows, input, audio and the GPU on each platform. Game code is the same
everywhere; no `#ifdef`s are needed for the supported platforms.

## Status

| Platform | Architectures | GPU backend | Status |
| --- | --- | --- | --- |
| Windows 10, 11 | x64 | Direct3D 12 (default), Vulkan | Built and tested on CI. Every CI run renders the isometric sample on Direct3D 12 through WARP (Microsoft's software renderer) and keeps the screenshot |
| macOS | Apple silicon, Intel | Metal | Built and tested on CI. The main development platform |
| Linux | x64 | Vulkan | Built and tested on CI (X11 and Wayland) |
| iOS 17+, iPadOS 17+ | arm64 | Metal | Builds, installs and launches (the `ios` preset; see [below](#ios-and-ipados)). It draws only on a device: SDL_GPU needs an A9 or newer GPU, and the iOS Simulator offers less. Not yet tried on a device |
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
| Direct3D 12 | HLSL from the shader bundle, compiled to DXBC when first loaded and cached on disk |
| Metal | MSL from the shader bundle |
| Vulkan | SPIR-V from the shader bundle |

Hardware and driver minimums are SDL_GPU's; see the
[SDL_CreateGPUDevice documentation](https://wiki.libsdl.org/SDL3/SDL_CreateGPUDevice) for each backend's.

The backend in use is printed at startup (`ready: 17 worker threads, metal backend`) and shown in the overlay.

## Why the entry point is a callback

SDL3 offers two ways to run a program: a classic `main()` with a loop, or callbacks (`SDL_AppInit`,
`SDL_AppIterate`, `SDL_AppEvent`, `SDL_AppQuit`) that SDL calls. The web, iOS and Android need the callback style,
because the operating system owns the loop there. The engine uses callbacks on every platform, through `BOWL_MAIN`, so
a game written on a desktop doesn't need changes later.

## High-DPI displays

Game coordinates are in logical points. On a high-DPI display (a Retina Mac, a 4K monitor at 200% scaling), the
window has more physical pixels than points: `window().size()` is in points, `window().pixel_size()` in pixels, and
`window().pixel_density()` is the ratio. Rendering happens at full physical resolution automatically; pass
`pixel_density()` to text and UI so glyphs rasterize sharply (see [text.md](text.md)).

## Platform notes

**Windows.** The default generator is Visual Studio, a multi-config generator: executables are in a
`RelWithDebInfo` or `Release` subfolder. The editor can't load a game's code library there yet (`--game`).

**macOS.** Release builds produce `.app` bundles; dev builds produce plain executables for faster iteration.
Installing signs bundles ad hoc; to ship without a Gatekeeper warning, sign with a Developer ID and notarize (see
[packaging](packaging.md#signing-and-notarization)).

**Linux.** SDL builds with X11 and Wayland support and picks one at runtime. Audio goes through PipeWire,
PulseAudio or ALSA, whichever is available.

## iOS and iPadOS

Games build for iPhone and iPad as they are: the same code, the same `bowl_add_game`. You need a Mac with Xcode.

1. Build the tools the build runs (the shader compiler, `bowl-icon`, `bowl-cook`, `bowl-pack`) for the Mac, once:

   ```bash
   cmake --preset host-tools
   ```

   ```bash
   cmake --build --preset host-tools
   ```

2. Make the Xcode project, and build for the simulator:

   ```bash
   cmake --preset ios
   ```

   ```bash
   cmake --build --preset ios-simulator
   ```

   Or open `build/ios/Bowl.xcodeproj` in Xcode, pick a game's scheme and a simulator, and run.

   The simulator is good for checking that a game builds, installs and starts, but it can't show it: SDL_GPU needs an
   iPhone or iPad GPU of the A9 generation or newer (Metal's Apple GPU family 3), and the simulator's GPU is less, so
   the game stops at startup with "Device does not meet the hardware requirements for SDL_GPU Metal". To see a game,
   run it on a device.

3. For a real iPhone or iPad, give the project your Apple development team (Xcode → Settings → Accounts shows its
   ID), then build with `--preset ios-device` or run from Xcode:

   ```bash
   cmake --preset ios -DBOWL_IOS_TEAM=ABCDE12345
   ```

Each game is an app with its `NAME`, `BUNDLE_ID` and `ICON` (as an asset catalog), runs full screen in landscape
(`bowl_add_game(... ORIENTATION portrait)` or `any` to change that), and needs iOS 17 or later. Its assets are packed
into the app as archives.

Things that work differently on a phone or tablet:

- **Touch.** `input.touches()` lists the fingers on the screen. Taps also count as mouse clicks, so buttons and the
  [UI](ui.md) work by touch without changes. `input.last_device()` is `Touch` after a touch. See [input](input.md).
- **The safe area.** `window().safe_area()` is the part of the screen clear of the notch, the rounded corners and the
  home indicator. Keep buttons and text inside it.
- **The background.** When the player switches away, the game stops updating and drawing (iOS ends apps that use the
  GPU in the background) and settings are saved; `engine.suspended()` is true until it comes back. iOS may close a
  game in the background without warning, so save the game when `SDL_EVENT_WILL_ENTER_BACKGROUND` reaches
  `Game::event`.
- **No dev tools.** iOS builds are like `release`: no overlay, no hot reload, precompiled shaders.

## Cross-compiling

To build for a platform that can't run the build tools (iOS, Android, consoles), build the tools for this machine
first (the `host-tools` preset puts `bowl-shaderc`, `bowl-icon`, `bowl-cook` and `bowl-pack` in `build/host-tools/bin`)
and point the target build at them:

```bash
cmake -B build/my-target ... -DBOWL_HOST_TOOLS=$PWD/build/host-tools/bin
```

The target build then uses those and builds none of its own. `BOWL_SHADERC_EXECUTABLE` points at just the shader
compiler, for builds that need nothing else.
