![License](https://img.shields.io/badge/License-Apache%202.0-red) ![Language](https://img.shields.io/badge/Language-C%2B%2B20-green)


# Bowl

**A C++20 game engine for building beautiful 2D games.**

Bowl is a custom 2D game engine built with C++ and SDL3. It supports top-down and isometric games, with a focus on performance, flexibility, and making complex games easier to build.

The engine handles graphics, audio, input, physics, saving, and other core systems so you can focus on making your game. It also includes an in-game development overlay, a level editor, and tools that let you change assets and game code while the game is running.

<p align="center">
  <img src="docs/images/isometric-night.jpg" width="49%" alt="Isometric game sample at night">
  <img src="docs/images/dev-overlay.png" width="49%" alt="Bowl development overlay">
</p>

## Features

### Graphics and Visuals
- Vulkan, Metal, and Direct3D 12 support.
- Smooth sprite animations and large numbers of on-screen objects.
- Dynamic 2D lighting, shadows, bloom, and other visual effects.
- Custom shaders, particles, and post-processing.
- Pixel art, sprite sheets, and animated tiles.
- Isometric and top-down maps, including large worlds.

### Gameplay Systems
- Entity and component system for organizing game objects.
- Collision detection, physics, and interaction systems.
- Keyboard, mouse, touch, and controller support.
- Menus, buttons, dialogue boxes, and other UI elements.
- Scene management and smooth transitions.
- Audio, music, and positional sound, with sound events that pick clips, volume and pitch for you.
- Save files, player settings, and world persistence.
- Optional multiplayer and networking support.
- Localization for multiple languages.

### Development Tools
- **Live asset updates:** Change textures, maps, and shaders without restarting the game.
- **Game code hot reload:** Update C++ code while keeping the current world loaded.
- **In-game developer overlay:** Inspect objects, monitor performance, view logs, and control game execution.
- **Bowl Editor:** Create levels, edit game data, work with pixel art, and make materials (glow and normal maps) and
  shaders, as code that compiles as you type or as a graph of nodes, previewed live under a light.
- **Attach to a running game:** Bowl Editor's Game tab shows its profiler, which systems read and write what, and its
  entities, whose components change as you edit them.
- **Built-in profiling:** Find performance problems and monitor how the engine runs.
- **Release packaging:** Tools for preparing builds for Windows, macOS, and Linux, with assets cooked for faster loading.

### Built for Expandability

Bowl is designed to support projects that grow over time. Its systems are modular, its gameplay code is separate from the engine's core, and its tools are designed to make experimentation and iteration easier.

It is especially suited to ambitious 2D games that combine exploration, simulation, crafting, combat, and detailed worlds.

## Getting Started

### Requirements
- CMake 3.25 or newer.
- A C++20 compiler.
- Windows, macOS, or Linux.

### Build the Engine

Clone the repository and run:

```bash
git clone https://github.com/SireSoul/Bowl.git
cd Bowl
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the isometric sample:

```bash
./build/dev/samples/isometric/isometric --night
```

See the [Getting Started Guide](docs/getting-started.md) for platform-specific instructions.

## Samples

| Sample | Description |
| --- | --- |
| **Tutorial** | A small playable game demonstrating menus, lighting, particles, sound, collisions, and saving. |
| **Isometric** | An explorable isometric world with lighting, shadows, dialogue, and a pause menu. |
| **Sprites** | A performance-focused demonstration featuring thousands of animated sprites, custom shaders, and GPU particles. |

<p align="center">
  <img src="docs/images/tutorial-play.png" width="32%" alt="Tutorial sample">
  <img src="docs/images/isometric-day.jpg" width="32%" alt="Isometric sample">
  <img src="docs/images/sprites.png" width="32%" alt="Sprites sample">
</p>

Prebuilt samples are available on the [Releases page](https://github.com/SireSoul/Bowl/releases).

## Documentation

- [Getting Started](docs/getting-started.md) — Installation and setup.
- [Tutorial](docs/tutorial.md) — Build a game step by step.
- [Architecture](docs/architecture.md) — How the engine works.
- [Documentation Index](docs/README.md) — Guides and API reference.
- [Troubleshooting](docs/troubleshooting.md) — Common problems.
- [Roadmap](docs/roadmap.md) — Development progress.

## Supported Platforms

| Platform | Graphics API | Status |
| --- | --- | --- |
| Windows 10/11 | Direct3D 12 / Vulkan | Supported |
| macOS | Metal | Supported |
| Linux | Vulkan | Supported |
| iOS / iPadOS | Metal | Experimental |
| Android | Vulkan | Planned |
| Web | WebGPU | Planned |

## Project Structure

- `engine/` — Core engine systems.
- `editor/` — Bowl's level and asset editor.
- `tools/` — Supporting development tools.
- `samples/` — Example games.
- `tests/` — Automated tests.
- `docs/` — Documentation.

## Project Status

Bowl is in early development. Its core systems, editor, and sample projects are actively evolving, and the API may change as the engine develops.

## Made with Bowl

Bowl is free to use for personal and commercial games under the Apache License 2.0. Attribution is appreciated.

If you make a game with Bowl, you can submit an issue with a screenshot and a link to have it featured here.

## License

Bowl is licensed under the [Apache License 2.0](LICENSE). See the [NOTICE](NOTICE) file for third-party licenses and attribution.

Copyright 2026 Starlight (SireSoul).
