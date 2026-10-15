# Dev tools

Dev builds (the `dev` and `debug` presets, or `PH_DEV_TOOLS=ON`) include an overlay built with
[Dear ImGui](https://github.com/ocornut/imgui). It runs inside the game, over the game, and needs no setup.

<p align="center"><img src="../images/dev-overlay.png" width="80%" alt="The overlay with the profiler and the entity inspector"></p>

**F1** shows and hides it. `--overlay` opens it at startup, and `--inspect N` opens it with entity index N selected.

Release builds (`PH_DEV_TOOLS=OFF`) contain none of this: no ImGui, no runtime shader compiler, no file watching.

## The main window

The top of the window shows the frame rate and CPU frame time with a graph of recent frames, the GPU backend, the
renderer's stats (sprites, draw calls, batches), entity and archetype counts, playing voices and worker threads, and a
**vsync** toggle.

### Time

| Control | |
| --- | --- |
| **Paused** | Stops fixed ticks; frame updates get a delta of 0 |
| **Step tick** | While paused, runs exactly one fixed tick |
| **Time scale** | 0.05x to 4x, for slow motion or fast forward |

The current tick, simulated time and interpolation alpha are shown underneath. Pause, step and slow motion make
simulation bugs reproducible: pause just before the problem, then step tick by tick.

### Profiler

The zones of the last frame, sorted by time, with call counts. Engine phases and every system appear automatically;
add your own with `PH_PROFILE_SCOPE("name")` (see [jobs-and-profiling.md](jobs-and-profiling.md#profiling)).

### Post & lighting

Every [post-processing](rendering.md#post-processing) setting as a live control: exposure, contrast, saturation,
tint, tonemapping, bloom (threshold, knee, intensity, radius), vignette and 2D lighting with the ambient color. Tune
here, then copy the values into code. The number of cached GPU pipelines is shown at the bottom.

### Systems

Both schedules, each system with its parallel batch number, its last run time and whether it runs exclusively. Each
system has a checkbox to switch it off, which is a quick way to see what a system is responsible for. The
**parallel** checkbox runs a schedule on one thread, in order, to rule out a race.

### Entities

Every archetype (the set of components its entities share) with its entities, the first 64 of each, with their
`ecs::Name` if they have one. Click one to inspect it.

The filter box narrows the list: every word must appear in one of an entity's component names or its name, ignoring
case, and `#42` finds entity 42. `player health` shows entities with both; `slime` finds every entity named or
componented "slime".
**Save world** writes every savable entity to `devtools_world.json` in the working directory; **Load (adds to
world)** loads that file back into the current world.

### Assets

Every loaded asset with its type and version, a **reload** button for each, and the **hot reload** switch.

### Log

Recent log lines, colored by level.

## The inspector

Selecting an entity opens its inspector window:

- Every component, by type name. Reflected components (`PH_COMPONENT`, `PH_REFLECT`) show their fields as editable
  controls: numbers, checkboxes, text, math vectors, colors with a color picker, nested reflected structs, and
  lists (`std::vector` and `std::array`, item by item; vectors get **+** and **-** to add or remove items at the end).
  Changes apply immediately. Fields of other types are listed with their type.
- Entity fields show the referenced entity as a button that jumps to it.
- Components that aren't reflected are listed by name.
- The **x** on a component's header removes it; **Add component** adds any savable component type the entity doesn't
  have, with its default values.
- **Duplicate** copies the entity with all its savable components; **Copy JSON** puts a
  [world document](saving.md#world-files) holding just this entity on the clipboard (its one entry under
  `"entities"` is a ready-made [prefab](saving.md#prefabs)); **Destroy** removes it.

Editing a value in the inspector while the game is paused, then stepping, is a quick way to test edge cases ("what
happens at 1 health?").

## Your own tools

`Game::dev_ui` (and `Scene::dev_ui` for the top scene) is called every frame while the overlay is open. Draw any Dear
ImGui windows there:

```cpp
#if PH_DEV_TOOLS
#include <imgui.h>
#endif

void dev_ui(Engine& engine) override {
#if PH_DEV_TOOLS
    ImGui::Begin("Spawner");
    ImGui::SliderInt("per tick", &spawn_rate_, 1, 2000);
    if (ImGui::Button("Spawn 10k")) spawn(engine, 10000);
    ImGui::End();
#else
    (void)engine;
#endif
}
```

Guard ImGui code with `#if PH_DEV_TOOLS` so release builds, which don't have ImGui, still compile. The sprites sample
has a working example.

Window positions and sizes are remembered between runs in `devtools.ini` in the working directory.

## Input while the overlay is open

When the overlay is using the keyboard (typing in a field) or the mouse (over a window, dragging), the game doesn't see
new presses from that device. Everything else reaches the game as usual, so you can keep playing with the overlay
open.

## Screenshots and automated runs

Not part of the overlay, but useful alongside it: `--screenshot out.png` saves a frame, overlay included, and
`--frames N` quits after N frames. Together they make quick visual checks from a script or CI. See
[reference/command-line.md](../reference/command-line.md).
