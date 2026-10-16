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
| Assets read from | your source folders first | next to the executable only, packed into `.bowlpak` archives (`BOWL_PACK_ASSETS`) |

Static SDL means the game has no SDL library to ship alongside it.

## Installing

```bash
cmake --install build/release --prefix dist
```

Every game target (every `bowl_add_game`) is laid out in `dist`:

**Windows and Linux**: one folder per game.

```
dist/
  my_game/
    my_game(.exe)
    engine.bowlpak   engine shaders and the default font
    assets.bowlpak   your assets, with compiled shaders
    bowl-LICENSE.txt Bowl's license and notice, which every copy of the game carries
    bowl-NOTICE.txt
```

**macOS**: one self-contained application bundle per game.

```
dist/
  my_game.app/
    Contents/
      MacOS/my_game
      Resources/engine.bowlpak
      Resources/assets.bowlpak
      Resources/bowl-LICENSE.txt
      Resources/bowl-NOTICE.txt
      Info.plist
```

The bundle's identifier defaults to `com.bowl.<target>`; set yours with
`bowl_add_game(my_game ... BUNDLE_ID com.yourstudio.mygame)`. The bundle version comes from the `project(... VERSION)`
in your `CMakeLists.txt`.

Without `BOWL_PACK_ASSETS` (the default outside the `release` preset), `engine/` and `assets/` are folders of loose files
instead of archives; the engine reads either (see [Assets](assets.md#archives)). `bowl-LICENSE.txt` and
`bowl-NOTICE.txt` stay loose either way, so players can read them. Build folders get them too, next to the executable
(in `Resources/` of a macOS bundle, at the top of an iOS app), and every installer and AppImage carries the installed
copy.

Zip the folder (or the `.app`) and it runs on another machine of the same OS and architecture. Or let
[`bowl_package`](#installers) make installers.

## The icon

Give `bowl_add_game` a picture and a name:

```cmake
bowl_add_game(my_game ASSETS assets NAME "My Game" ICON art/icon.png)
```

The icon is a square PNG, 1024 x 1024 at best. The build turns it into what each platform wants:

| | |
| --- | --- |
| macOS | `my_game.icns` with every size from 16 to 1024, in the bundle's `Resources/` |
| Windows | `my_game.ico` with 16 to 256, built into `my_game.exe` (the icon Explorer shows) |
| Linux | `my_game.png` (256 x 256) and `my_game.desktop` next to the game, for menus, AppImage and Flatpak |
| Everywhere | the window's icon (title bar, taskbar; the Dock when the game isn't an `.app`) |

A small pixel-art icon scales up blurry; add `ICON_PIXEL_ART` to keep its pixels hard-edged. The samples use
`samples/icon.png`, 64 x 64, that way.

The `bowl-icon` tool does the work, and you can run it yourself:

```bash
bowl-icon icon.png --icns my_game.icns --ico my_game.ico --png 512 my_game-512.png
```

## Signing and notarization

### macOS

Installing signs each `.app` with `codesign`. By default the signature is ad hoc (`BOWL_MACOS_SIGN_IDENTITY` is `-`):
the bundle is sealed, so nothing in it can change unnoticed, but it doesn't say who made it. That's enough on your own
Mac. On other Macs, Gatekeeper warns about an app from the internet unless it's signed with a Developer ID and
notarized by Apple.

To ship, join the Apple Developer Program, then:

1. Sign with your Developer ID. Installing then also turns on the hardened runtime and a secure timestamp, which
   notarization needs:

   ```bash
   cmake --preset release -DBOWL_MACOS_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
   ```

2. Store your notarization credentials once (an app-specific password, or an App Store Connect API key):

   ```bash
   xcrun notarytool store-credentials my-profile --apple-id you@example.com --team-id TEAMID
   ```

3. Have Apple check the app or disk image, and staple the result to it:

   ```bash
   cmake -DFILE=build/release/MyGame-1.2.0-Darwin.dmg -DPROFILE=my-profile -P cmake/BowlNotarize.cmake
   ```

`BOWL_MACOS_ENTITLEMENTS` takes an entitlements file, if the game needs any. Set `BOWL_MACOS_SIGN_IDENTITY` to empty to
skip signing.

### Windows

Windows SmartScreen warns about unsigned programs. With a code-signing certificate, set `BOWL_WINDOWS_SIGN_COMMAND` to
the command that signs a file; installing runs it on each game's `.exe`:

```bash
cmake --preset release -DBOWL_WINDOWS_SIGN_COMMAND="signtool;sign;/fd;SHA256;/a;/tr;http://timestamp.digicert.com;/td;SHA256"
```

## Installers

`bowl_package` in your `CMakeLists.txt`, after `bowl_add_game`, sets up installers for the game:

```cmake
bowl_package(my_game VENDOR "Your Studio" DESCRIPTION "A game about bowls" LICENSE LICENSE.txt)
```

Build, then run `cpack` in the build folder:

```bash
cmake --build --preset release
```

```bash
cd build/release && cpack
```

| OS | Makes |
| --- | --- |
| macOS | `MyGame-1.2.0-Darwin.dmg`: the signed `.app` next to a link to Applications, with the icon on the volume. Also a `.zip` |
| Windows | `MyGame-1.2.0-Windows.exe`, an installer with a Start menu entry and an uninstaller, when [NSIS](https://nsis.sourceforge.io) is installed. Also a `.zip` |
| Linux | `MyGame-1.2.0-Linux.tar.gz` |

Each comes with a `.sha256` file. `cpack` installs and signs the game the same way `cmake --install` does.

### AppImage

An AppImage is one file that runs on most Linux distributions:

```bash
cmake --build build/release --target my_game_appimage
```

This installs the game into `build/release/my_game.AppDir`, adds what an AppImage needs (`AppRun`, the desktop file and
the icon, so the game needs an `ICON`), and packs it with
[appimagetool](https://github.com/AppImage/appimagetool/releases) if that's on `PATH`. Without it, the AppDir is left
ready for you to pack. Build on the oldest distribution you support: an AppImage runs on systems as new as or newer
than the one it was built on.

### Flatpak

`bowl_package` writes `build/release/<bundle id>.flatpak.json`, a manifest for `flatpak-builder`. It builds the game from
your source folder with the freedesktop runtime, and installs the desktop file, the icon and Bowl's license files
where Flatpak looks (`BOWL_LINUX_SHARE`; the license files go in `share/licenses/<bundle id>/`):

```bash
flatpak-builder --user --install --force-clean build/flatpak build/release/com.yourstudio.mygame.flatpak.json
```

The manifest lets the build download the engine's dependencies, which a local build allows and Flathub doesn't; for
Flathub, list them as sources instead. If the engine lives outside your project's folder, add it as a source too. The
game gets the display, the GPU, sound and gamepads; add `--share=network` to `finish-args` if it plays online.

## Steam Deck

On a Steam Deck (Steam says so, or the hardware is one), `bowl::is_steam_deck()` is true and the game starts fullscreen
(turn that off with `AppConfig::fullscreen_on_steam_deck`). To see how a game looks there, run it with `--deck`: a
1280 x 800 window that also makes `is_steam_deck()` true.

Valve's checks for "Deck Verified", and what they mean here:

- **Controls.** Everything works with a gamepad, including menus and text entry. Bind gamepad buttons for every
  action (see [Input](input.md)); the [UI](ui.md) navigates by focus.
- **Glyphs.** Show gamepad buttons, not keys, when the player uses a gamepad (`input.last_device()`).
- **Text.** At 1280 x 800, text is at least 9 pixels tall, 12 is better. Check with `--deck`.
- **Resolution.** The game runs at 1280 x 800 (16:10) without black bars where possible, or at 16:9 letterboxed.
- **No launcher**, and no mouse needed anywhere.

The Deck runs Linux builds directly and Windows builds through Proton. A Linux build is the more direct path. Test on a
real Deck before shipping: performance, suspend and resume, and the on-screen keyboard can't be checked in a window.

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
- **Bowl's license.** The Apache License 2.0 asks every copy of your game to carry Bowl's `LICENSE` and `NOTICE`,
  and the build does it: each game gets `bowl-LICENSE.txt` and `bowl-NOTICE.txt` next to its executable (in
  `Resources/` of a macOS bundle), loose even when assets are packed, and installs, installers, AppImages and Flatpaks
  include them (see [Installing](#installing)). Still yours to do: mark engine files you changed, and ship the
  licenses of the libraries your build uses (the README's License section lists them).
- **Check a clean machine.** Run the installed copy, not the build folder: it proves every asset was installed.

## Not done yet

These are on the [roadmap](../roadmap.md):

- Android packaging. (iOS apps come from the `ios` preset; see [platforms](platforms.md#ios-and-ipados).)
