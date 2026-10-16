# Troubleshooting

Problems people run into, grouped by when they happen. If yours isn't here, the log usually says what went wrong:
read it from the top, since the first error tends to cause the rest.

- [Configuring](#configuring)
- [Building](#building)
- [Starting the game](#starting-the-game)
- [Drawing](#drawing)
- [Assets and hot reload](#assets-and-hot-reload)
- [Game-code hot reload](#game-code-hot-reload)
- [ECS](#ecs)
- [Input and audio](#input-and-audio)

## Configuring

**`CMake 3.25 or higher is required`**
Update CMake. On Windows, the copy bundled with Visual Studio 2022 is new enough if Visual Studio is up to date. On
macOS, `brew upgrade cmake`. On Linux, Kitware's APT repository or `pip install cmake` gives recent versions.

**A download fails, or `HASH mismatch`**
Dependencies are downloaded from GitHub on the first configure. Check the network (and any proxy), then delete the
build folder and configure again. A hash mismatch on an unchanged checkout means the download was corrupted or
intercepted; don't change the hash to make it pass.

**SDL3 isn't found, and building it fails on Linux**
SDL3 needs development headers for X11, Wayland and audio; install the packages listed in
[getting-started.md](getting-started.md#linux). Delete the build folder after installing them, because CMake caches
what it found.

**`hot reload needs CMake 3.27+; building it normally`**
A warning, not an error. Everything works except game-code hot reload. Update CMake to get it.

## Building

**Errors about `std::format`, `<numbers>`, concepts or `requires`**
The compiler is too old, or isn't in C++20 mode. You need Visual Studio 2022, Xcode 15, Clang 16 or GCC 13. If your
own game target sees these errors in engine headers, make sure it links `bowl::engine` (directly or through
`bowl_add_game`), which carries the C++20 requirement.

**The first build takes a long time**
It compiles SDL3 (unless installed), glslang and SPIRV-Cross. Later builds don't. On macOS and Linux, installing
`ccache` and configuring with `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache` makes rebuilds
of a clean build folder fast too.

**Warnings fail the build**
`BOWL_WARNINGS_AS_ERRORS` is on. Fix the warning (CI builds with it on for Linux and macOS), or configure with
`-DBOWL_WARNINGS_AS_ERRORS=OFF` locally.

**A shader fails to compile during the build**
The build prints the compiler's message with file and line. A common cause is a resource in the wrong descriptor set;
see [shaders.md](guides/shaders.md#resource-bindings).

## Starting the game

**`SDL_Init failed` or no window appears**
On Linux, check that a display server is running (`echo $DISPLAY $WAYLAND_DISPLAY`). Over SSH, there's no display.

**`gpu: ... failed` or no GPU device**
The GPU or its driver doesn't support the backend. Try another with `--driver vulkan` or `--driver direct3d12`. On
Linux, install a Vulkan driver (`mesa-vulkan-drivers` for AMD and Intel) and check `vulkaninfo`. In a virtual machine
without GPU acceleration, Vulkan may not be available at all.

**The game starts, but the log says it can't find `engine/...` assets**
The `engine/` folder isn't next to the executable. Build the whole game target (not just the source files) so its
`<game>_assets` step runs, or run the installed copy.

**Settings or controls from a previous run aren't applied**
Check the log line that says where settings are stored. Settings are saved when the game exits normally; killing the
process (or a crash) loses changes made since the last `settings().save()`.

## Drawing

**Nothing is drawn**
Check that the batch is begun every frame (`batch.begin(camera)`) and submitted (`renderer().submit(batch)`). Check
where the camera looks: `engine.camera()` starts centered on the window, so world point (0, 0) is at the top-left
corner of the screen until you move the camera.

**Things are drawn in the wrong order**
Batches sort by depth by default. Give the ground a lower depth than everything on it, and give characters depth by
their feet's y. For UI, begin the batch with `SpriteSort::None` to keep call order. Batches always draw in the order
they're submitted.

**UI or text is in the wrong place, or keeps adding up**
The UI batch must be begun with a screen camera each frame before `ui.begin`:
`batch.begin(gfx::Camera2D::screen(engine.window().size()), gfx::SpriteSort::None)`. A batch that's never begun keeps
every sprite from every frame.

**Text is blurry**
Pass `raster_scale = engine.window().pixel_density()` in the `TextStyle` (times the camera zoom for text in the
world). See [text.md](guides/text.md#sharp-text-raster_scale).

**Everything is too dark after turning on lighting**
With `post().lighting` on, the scene is multiplied by `ambient` plus the light map. Raise `ambient`, or draw lights
with `submit_light`. UI should go through `submit_ui`, which isn't lit.

**Bloom makes everything glow**
Raise `post().bloom_threshold` above the brightness of your normal art (1.0 means white), and use colors above 1 only
for things meant to glow.

**Pixel art looks blurry or shimmers**
Set `AppConfig::pixel_art = true`, and zoom by whole numbers.

## Assets and hot reload

**An asset handle is empty**
The log names the file and the reason. The path is virtual and relative to your assets folder (`"characters/hero.png"`,
not `"assets/characters/hero.png"`), with forward slashes. On Linux, paths are case-sensitive.

**Editing a file doesn't update the game**
Hot reload is a dev-build feature: check that the build has `BOWL_DEV_TOOLS=ON` and that the overlay's **Assets** panel
has **hot reload** ticked. The game must read from your source folder, which `bowl_add_game(... ASSETS dir)` sets up;
editing the copy in the build folder does nothing. Some editors save by writing a new file and renaming it, which is
fine; the change is picked up within a quarter of a second.

**A shader edit doesn't show**
If it failed to compile, the log has the error and the old version is still in use. Fix and save again.

## Game-code hot reload

**Rebuilding doesn't reload anything**
Rebuild the library target, `<game>_code`, and make sure the game was started from the host executable of the same
build folder. Check the log for `hot reload: watching ...` at startup; if it's missing, hot reload isn't on (Windows,
release build, CMake older than 3.27, or `HOT_RELOAD` missing from `bowl_add_game`).

**`the new build didn't load; still running the previous code`**
The library failed to load. The lines before it say why: usually a missing symbol. The game keeps running the old
code; fix the problem and rebuild.

**Entities lose a component after reloading**
That component isn't savable. Use `BOWL_COMPONENT`, or rebuild the component in `init` (see
[hot-reload.md](guides/hot-reload.md#writing-reloadable-code)).

**State resets after reloading**
State kept as members of the `Game` class is lost; the `Game` is created again. Keep it in the world as a resource or
component.

**Something happens twice after a reload**
Code in `init` that has side effects beyond setup (spawning entities, pushing scenes, playing music) runs again on
every reload. Move it to `start`.

## ECS

**`structural change while iterating: use a CommandBuffer`**
You created, destroyed, or added or removed a component while a query was running. Record the change in
`world.deferred()` or `ctx.commands` instead. See [ecs.md](guides/ecs.md#changing-structure-while-iterating).

**`entity is dead or lacks the component`**
`get<T>(e)` asserts when the entity is dead or doesn't have `T`. Use `try_get<T>(e)` when it might not.

**A component doesn't save, or loads as unknown**
Savable components are reflected (`BOWL_COMPONENT`), have hooks, or are empty tags. A type must be registered before
loading: `BOWL_COMPONENT` does that; otherwise call `ecs::register_components<T>()`. Renaming a type or moving it to
another namespace changes its saved name.

**Results differ between runs**
Look for systems that read or write data they didn't declare, and for randomness without a fixed seed. Turn off
**parallel** for the schedule in the overlay: if the problem disappears, it's a race between systems.

## Input and audio

**A key press is missed or counted twice**
Read `pressed` in the same context you act on it: in `fixed_update` for gameplay. See
[input.md](guides/input.md#edges-and-fixed-ticks).

**The game ignores the keyboard while the overlay is open**
The overlay has keyboard focus (a text field, or a window being dragged). Click on the game area, or close the overlay.

**No sound**
Check the log for `audio: no output device`. Check bus gains (`audio().bus_gain(audio::Bus::Master)`), and that the
sound asset loaded.
