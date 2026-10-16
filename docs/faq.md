# FAQ

## General

**What kind of games is it for?**
2D games: top-down, isometric, side-on, with a lot happening on screen and a look that benefits from HDR, bloom, 2D
lighting and custom shaders, and that need the unglamorous parts done properly: tilemaps, dialogue, saving, input
remapping, menus, localization.

**Does it do 3D?**
No. The renderer, math and tools are 2D. Isometric games are 2D with y-sorting.

**Is there an editor?**
Not a scene editor. Maps come from Tiled, animations from Aseprite, and the in-game overlay inspects and edits
entities while the game runs. An optional editor built on the engine is on the [roadmap](roadmap.md); code will stay
the primary way to make games.

**Is there scripting?**
No, and none is planned. Game code is C++, and game-code hot reload rebuilds and swaps it in when you save a file,
which is the quick iteration a scripting language would otherwise be for.

**Why "Bowl"?**
The full name is Bowl Engine; everyone calls it Bowl. In code it's the namespace `ph`.

## Design

**Why an ECS?**
Games with many similar things (enemies, projectiles, particles, crops, NPCs) spend most of their time doing the same
work on each. Storing each component type in its own packed array makes that work fast and easy to spread across
cores, and keeps game data in plain structs that can be saved, inspected and hot-reloaded generically. You don't have
to use it for everything: the `Game` class and scenes can hold plain C++ state too.

**Why are components plain structs instead of classes with methods?**
Plain data can be copied, saved, loaded, shown in an inspector and moved between archetypes without the component's
cooperation. Behavior lives in systems, which see many components at once.

**Why a fixed timestep?**
So the game behaves the same at 30 fps and at 240 fps, and so simulations are reproducible. Rendering interpolates
between ticks, so motion stays smooth anyway. See [game-loop.md](guides/game-loop.md#fixed-timestep).

**Why does the engine own `main()`?**
SDL3's callback entry points are required on the web, iOS and Android, where the operating system runs the loop. Using
them everywhere means a game written on a desktop runs on those platforms without restructuring.

**Why SDL_GPU and not Vulkan, OpenGL or bgfx directly?**
SDL_GPU gives modern explicit GPU access (command buffers, pipelines, storage buffers) on Vulkan, Metal and Direct3D
12 through one API, maintained alongside SDL itself, which the engine already uses for everything else. OpenGL is
deprecated on macOS and missing on consoles.

**Why GLSL, and not HLSL or a custom shading language?**
GLSL compiles to SPIR-V with the reference compiler (glslang), and SPIRV-Cross turns that into MSL and HLSL. Writing
Vulkan-style GLSL means writing each shader once.

**Why JSON for saves and settings, not a binary format?**
Saves you can read and diff make bugs easier to find, and JSON keeps working as components gain fields. A binary
format for very large worlds is on the roadmap.

**Why no exceptions?**
Most failures in a game are expected (a missing file, a bad save) and are better handled where they happen, with the
game carrying on. Programming errors are caught by assertions in dev builds.

## Practical

**How do I make a game without putting it in the engine repository?**
Add the engine as a submodule or with FetchContent and call `ph_add_game`. See
[reference/cmake.md](reference/cmake.md#using-the-engine-in-your-own-project).

**How do I get a value into the inspector?**
Put it in a component with `PH_COMPONENT(Type, fields...)`. For values that aren't per-entity, draw your own window
in `Game::dev_ui`.

**How many entities can it handle?**
Hundreds of thousands of simple entities update in a few milliseconds when systems run in parallel. Drawing is usually
the limit before the ECS is: tens of thousands of sprites per frame are fine; more depends on the GPU. Measure with
the overlay and `ph_bench_ecs`.

**Can I use my own renderer, or skip the ECS?**
The modules are separate libraries. Tools can link only `ph::core` and `ph::ecs`, for example. A game is expected to use
the `app` module, which brings the renderer and the rest, but nothing forces a game to create entities.

**How do I add a third-party library?**
Like in any CMake project: `FetchContent` or `find_package`, then `target_link_libraries(my_game PRIVATE ...)`. With
game-code hot reload, your code lives in `<game>_code`, so link it there too when that target exists:

```cmake
target_link_libraries(my_game PRIVATE box2d)
if(TARGET my_game_code)
    target_link_libraries(my_game_code PRIVATE box2d)
endif()
```

**Is the API stable?**
Not yet. It changes when the first game needs it to; [CHANGELOG.md](../CHANGELOG.md) records the changes.
