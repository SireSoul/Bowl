# Changelog

Notable changes to the engine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Until
1.0, any release may change the API; changes that break existing games are marked **Breaking**.

## [Unreleased]

### Added
- `tutorial` sample: Fireflies, the game `docs/tutorial.md` builds step by step. CI compiles it, so the tutorial stays
  correct.
- `update_particle_emitters` and `draw_particle_emitters` run every `gfx::ParticleEmitter` in a world. The component
  is now registered with `PH_COMPONENT`.
- `GpuDevice::update_texture_region` uploads one rectangle of a texture.
- `Input::unbind_all`.
- `Vec2::distance`, `Vec2::angle`, `Rect::from_center`, `Rect::expanded`.
- Documentation: getting started, a tutorial, a guide per system, CMake and command-line reference, conventions,
  troubleshooting, FAQ, contributing guide, issue and pull request templates.

### Changed
- Font atlases upload only the rectangle holding newly added glyphs, instead of the whole 4 MB page.
- `ph_gfx` depends on `ph_ecs` (for the particle emitter helpers).
- Every engine target carries C++20 as a usage requirement, so games that add the engine with `add_subdirectory`
  compile its headers correctly without setting `CMAKE_CXX_STANDARD` themselves.
- `Settings::save` is no longer `const`; `Settings::slot` marks settings as changed.
- Comments throughout the code were rewritten to say plainly what the code does, and outdated ones corrected.

### Fixed
- Game-code hot reload no longer duplicates input bindings each time `init` runs again; a changed default binding
  now replaces the old one.
- The `ph_tests` executable no longer links every module twice (a macOS linker warning).

## [0.1.0] - 2026-10-14

The first version: everything needed to start making a game.

### Added
- **Core:** math, easing and tweens, logging, assertions, a job system with deterministic chunking, a CPU profiler,
  file watching, JSON with comments and trailing commas, UTF-8, reflection (`PH_REFLECT`), JSON serialization, player
  settings, localization with fallbacks and plurals.
- **Assets:** virtual paths and mounts, shared `Asset<T>` handles, hot reload that keeps the last good version on
  failure, custom loaders.
- **ECS:** archetype storage, cached queries, `par_each`, command buffers, hooks, resources, a parallel scheduler with
  deterministic results, world save and load, prefabs, entity references remapped on load, parent/child transforms.
- **Rendering:** SDL_GPU on Vulkan, Metal and Direct3D 12; instanced sprite batches with one upload per frame;
  cameras; depth and texture sorting; blend modes; custom materials; HDR, bloom, color grading, ACES tonemapping,
  vignette; 2D lighting; a UI layer; screenshots.
- **Shaders:** `ph-shaderc` compiles GLSL to SPIR-V, MSL and HLSL bundles; dev builds compile and reload shaders while
  the game runs.
- **Content:** TrueType text, Aseprite sprite sheets and animation, Tiled maps (orthogonal, isometric, y-sorted,
  animated tiles), CPU particles.
- **Audio:** a 128-voice mixer with buses, WAV and OGG sounds, streamed OGG music with gapless loops and crossfades.
- **Input:** keyboard, mouse, four gamepads, named actions with edges that work in fixed ticks, rebinding saved to
  settings.
- **Game UI:** panels, buttons, checkboxes, sliders, progress bars, nine-slice art, focus navigation, a dialogue box.
- **App:** `Game` and `Engine`, a fixed-timestep loop with interpolation, a scene stack with fades, SDL3 callback entry
  points.
- **Collision:** overlap and separation, swept boxes, raycasts, a spatial hash, `move_and_slide`.
- **Dev tools:** the F1 overlay with profiler, systems, entity inspector, assets, log and time controls.
- **Game-code hot reload** on macOS and Linux.
- **Packaging:** release builds with static SDL, `.app` bundles on macOS, install layouts.
- **Samples:** `isometric` and `sprites`.
- **CI:** Windows, macOS and Linux, `dev` and `release`, tests, and a Direct3D 12 render on Windows.

[Unreleased]: https://github.com/SireSoul/PLACEHOLDERENGINENAME/compare/main...develop
[0.1.0]: https://github.com/SireSoul/PLACEHOLDERENGINENAME/tree/main
