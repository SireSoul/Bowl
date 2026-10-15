# Packaging and shipping

Shipping a game means a `release` build installed into a folder you can zip, upload or wrap in an installer.

## The release preset

```bash
cmake --preset release
```

```bash
cmake --build --preset release
```

What it changes from `dev`:

| | `dev` | `release` |
| --- | --- | --- |
| Optimization | `-O2` with debug info | full optimization, no debug info |
| Dev overlay, ImGui | yes | no |
| Shaders | precompiled, and compiled from source at runtime | precompiled only |
| Asset hot reload | yes | no |
| Assertions | on | off |
| SDL | installed copy if present, else static | always built from source and linked statically |
| macOS | plain executables | `.app` bundles |
| Assets read from | your source folders first | next to the executable only |

Static SDL means the game has no SDL library to ship alongside it.

## Installing

```bash
cmake --install build/release --prefix dist
```

Every game target (every `ph_add_game`) is laid out in `dist`:

**Windows and Linux**: one folder per game.

```
dist/
  my_game/
    my_game(.exe)
    engine/        engine shaders and the default font
    assets/        your assets, with compiled shaders
```

**macOS**: one self-contained application bundle per game.

```
dist/
  my_game.app/
    Contents/
      MacOS/my_game
      Resources/engine/
      Resources/assets/
      Info.plist
```

The bundle's identifier defaults to `com.bowl.<target>`; set yours with
`ph_add_game(my_game ... BUNDLE_ID com.yourstudio.mygame)`. The bundle version comes from the `project(... VERSION)`
in your `CMakeLists.txt`.

Zip the folder (or the `.app`) and it runs on another machine of the same OS and architecture.

## What players need

| OS | Needs |
| --- | --- |
| Windows 10 or 11 | A GPU with Direct3D 12 or Vulkan drivers. `d3dcompiler_47.dll` ships with Windows. The Visual C++ runtime is needed unless you link it statically |
| macOS | A Mac that SDL_GPU's Metal backend supports |
| Linux | A Vulkan driver, and X11 or Wayland. SDL loads the windowing and audio libraries it needs at runtime, so they don't have to be present at build time on players' machines |

## Before you ship

- **Organization name.** Set `AppConfig::organization` (and `application` if the title isn't a good folder name).
  Settings and saves live in a folder named after them; changing them after release moves players' saves.
- **Version.** Set `project(my_game VERSION 1.2.0 ...)`; it goes into the macOS bundle.
- **Logging.** Release builds log at `Info` by default; consider `Warn` in `AppConfig::log_level`, or add a log sink
  that writes a file players can send you.
- **Check a clean machine.** Run the installed copy, not the build folder: it proves every asset was installed.

## Not done yet

These are on the [roadmap](../roadmap.md):

- application icons (Windows `.ico`, macOS `.icns`);
- macOS code signing and notarization (until then, Gatekeeper warns about the app on other Macs);
- a Windows installer, a Linux AppImage or Flatpak;
- iOS and Android packaging;
- packing assets into an archive (they're loose files today).
