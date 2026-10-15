# Changelog

Notable changes to the engine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Until
1.0, any release may change the API; changes that break existing games are marked **Breaking**.

## [Unreleased]

### Added
- Sprite sheets: Aseprite slices (`SpriteSlice`, `slice_rect`, `draw_nine_slice` from a slice), animation events from
  tag user data (`SpriteAnimator::fired`), frame names (`frame_index`). `SpriteBatch::draw_nine_slice` from part of a
  texture.
- `ph-atlas`: packs a folder of PNGs into an atlas with sprite-sheet JSON (trimming, padding, numbered files become
  animations). The packer is the `ph_atlas` library.
- Collision: `Capsule` and convex `Polygon` shapes, and `Shape` (any of the four) with `collide`, `overlaps`,
  `contains`, `raycast` and `bounds` for every pair.
- `physics::Collider` component, `update_triggers` and the `TriggerEvents` resource (entered, inside, exited, with
  layers), and `overlapping` queries. `ph_physics` now depends on `ph_ecs`.
- Optional Box2D v3 rigid bodies (`-DPH_BOX2D=ON`): `RigidBody`, `PhysicsSettings`, `step_physics`, `apply_impulse`.
- Bowl Editor (`bowl-editor`): lays out levels in `.world` files. Art shelf with thumbnails, place and select tools,
  box select, groups, align, copy and paste, undo and redo, autosave, an inspector with cards, and the game's own
  components through `--game`. Unknown components are kept as JSON so saving never loses data.
- `gfx::Sprite`, `gfx::draw_sprites` and `gfx::sprite_bounds`: art on an entity, drawn at its `GlobalTransform2D`.
- `ecs::WorldFile`: `.world` files as hot-reloading assets. `ecs::Name`: a readable name for an entity.
- `World::remove_component` by id. Asset fields are editable through reflection (`FieldVisitor::asset`).
- `ph/app/inspector.hpp`: the overlay's component editor, shared with tools. `AppConfig::dev_ui_always` for tools
  built on the engine (their overlay key moves to F12).
- Game-code hot reload on Windows; CI runs a real reload there on every push.
- Game-code hot reload rebuilds the code by itself when a source file is saved; compile errors go to the log and the
  previous code keeps running. `--no-auto-build` turns it off. `AutoBuilder` does the watching and building.
- Tracy support (`-DPH_TRACY=ON`): profiler zones with source locations, frame marks, worker thread names, log
  lines and plots show up in the Tracy profiler. `profiler::plot` and `profiler::set_thread_name`.
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
