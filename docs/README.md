# Documentation

Everything about using and working on Bowl. If you're new, read these in order:

1. [Getting started](getting-started.md): install a toolchain, build, run the samples and tests
2. [Tutorial](tutorial.md): build a complete small game, one feature at a time
3. [Architecture](architecture.md): how the pieces fit together, and the rules they follow

After that, use the guides as you need them.

## Guides

One page per system.

| Guide | What's in it |
| --- | --- |
| [The game loop](guides/game-loop.md) | `Game` callbacks, fixed timestep and interpolation, `AppConfig`, time controls |
| [ECS](guides/ecs.md) | Entities, components, queries, command buffers, hooks, resources, hierarchy |
| [Systems and scheduling](guides/systems.md) | Schedules, parallel systems, declared access, barriers, determinism |
| [Rendering](guides/rendering.md) | Sprite batches, cameras, sorting, blend modes, materials, post-processing, 2D lighting |
| [Shaders](guides/shaders.md) | Writing GLSL for the engine, descriptor sets, `ph-shaderc`, live editing |
| [Assets](guides/assets.md) | Virtual paths, mounts, loaders, `Asset<T>`, hot reload, custom asset types |
| [Text](guides/text.md) | Fonts, `draw_text`, sizing on high-DPI screens, wrapping, typewriter reveals |
| [Sprite sheets and animation](guides/sprite-animation.md) | Aseprite export, `SpriteSheet`, `SpriteAnimator`, `draw_frame` |
| [Tilemaps](guides/tilemaps.md) | Tiled maps, layers, objects, properties, isometric y-sorting, tile collision |
| [Particles and tweens](guides/particles-and-tweens.md) | `ParticleSystem`, `ParticleEmitter`, easing curves, `Tween<T>` |
| [Audio](guides/audio.md) | Sounds, music, voices, buses, crossfades |
| [Input](guides/input.md) | Devices, actions, fixed-tick edges, rebinding, saving controls |
| [Game UI](guides/ui.md) | Immediate-mode widgets, focus navigation, styles, nine-slice art, dialogue |
| [Scenes](guides/scenes.md) | The scene stack, transparency, fades |
| [Collision](guides/collision.md) | Shapes, contacts, sweeps, raycasts, `move_and_slide`, the spatial hash |
| [Saving and loading](guides/saving.md) | Reflection, serialization, world files, prefabs, player settings |
| [Localization](guides/localization.md) | String tables, fallbacks, placeholders, plurals |
| [Jobs, logging and profiling](guides/jobs-and-profiling.md) | `JobSystem`, `parallel_for`, logging, profiler zones |
| [Dev tools](guides/dev-tools.md) | The F1 overlay, the inspector, your own tool windows |
| [Hot reload](guides/hot-reload.md) | Assets, shaders and game code while the game runs |
| [Packaging and shipping](guides/packaging.md) | Release builds, install layouts, macOS bundles |
| [Platforms](guides/platforms.md) | Backends, OS support, what's planned |
| [Performance](guides/performance.md) | Where time goes and how to keep it low |

## Reference

| Page | What's in it |
| --- | --- |
| [CMake](reference/cmake.md) | Options, presets, `ph_add_game`, `ph_compile_shaders`, using the engine from your own project |
| [Command line](reference/command-line.md) | Flags every game understands, and `ph-shaderc`'s |
| [Project layout](reference/project-layout.md) | Every folder and what lives there; what a built game looks like on disk |
| [Conventions](reference/conventions.md) | Coordinates, units, colors, naming, threading, error handling |

The headers are the API reference. Each public type and function has a comment saying what it does; start with
`engine/app/include/ph/app/engine.hpp`.

## Help

- [Troubleshooting](troubleshooting.md): build errors, black screens, missing assets, hot reload problems
- [FAQ](faq.md): design decisions and common questions

## Project

- [Roadmap](roadmap.md): what's done, what's next, known limitations
- [Contributing](../CONTRIBUTING.md): workflow, code style, comment style, tests
- [Changelog](../CHANGELOG.md)
