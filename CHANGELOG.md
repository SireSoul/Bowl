# Changelog

Notable changes to the engine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Until
1.0, any release may change the API; changes that break existing games are marked **Breaking**.

## [Unreleased]

### Added
- Compute shaders: `ph-shaderc` compiles `.comp.glsl` for every backend in SDL_GPU's compute layout;
  `gfx::ComputeShader` assets, `gfx::GpuBuffer`, `Renderer::dispatch` (`ComputeDispatch`), and
  `Renderer::submit` for sprites a compute shader wrote. `gfx::GpuParticles`: particles simulated and drawn on the GPU
  (the sprites sample's `--gpu-particles`).
- Direct3D 12 keeps the DXBC it compiles from HLSL in `shader-cache/` in the user data folder
  (`GpuDeviceDesc::shader_cache`, `load_cached_shader`, `store_cached_shader`).
- Look: your own full-screen effects (`Renderer::post_effects`, `PostEffect`, `PostStage::Scene` and `Final`),
  color-grading lookup tables (`PostSettings::lut`, `make_neutral_lut`), chromatic aberration and film grain;
  emissive sprites (`submit_emissive`); normal-mapped lighting and shadows from occluders (`light_material`,
  `submit_normals`, `submit_occluders`). The isometric sample's torch casts shadows; the sprites sample has `--film`.
- Saving: `ecs::Prefab` assets (`.prefab`, one entity or several) and `ecs::instantiate`; a binary document form
  (`json::encode_binary`, `decode_binary`, `parse_any`) read by every world loader and Bowl Editor; save-file
  migrations (`ecs::Migrations`, `serial::Context::game_version`, `rename_component`, `rename_field`,
  `remove_component`, `for_each_saved`). `json::Value::find` (mutable) and `erase`.
- Audio: bus effects (`set_bus_effects`: low-pass, high-pass, reverb, all smoothed), positional sounds (`play_at`,
  `set_position`, `set_listener`, `Attenuation`, `spatialize`), and Ogg Opus sounds and music with `-DPH_OPUS=ON`.
- Tilemaps: Tiled XML maps and tilesets (`.tmx`, `.tsx`, `Tilemap::parse_tmx`), staggered and hexagonal maps
  (`Tilemap::cell_at`, `pixel_size`), and `TilemapCache`, which keeps the flat layers of big maps in GPU memory as
  chunks. `TilemapDrawOptions::layers` draws only the flat or the y-sorted layers.
- `gfx::StaticSprites` and `Renderer::submit(StaticSprites, camera)`: sprites uploaded once and drawn every frame
  without being sent again. `gfx::sprite_order`.
- `ph/core/xml.hpp`: a small XML reader.
- Text: outlines and shadows (`TextStyle::outline`, `shadow`), rich text markup (`[color=#hex]`, `[wave]`, `[shake]`),
  fallback fonts (`Font::set_fallbacks`, `has_glyph`), distance-field text (`TextStyle::sdf`, `sdf_text_material`), and
  `layout_text` for drawing laid-out characters yourself. `Font::advance` measures without rasterizing.
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
- `.phsh` bundles are format version 2 (compute fields); version 1 bundles still load.
- Lighting is applied in its own pass before bloom (so scene effects see the lit image); bloom still comes from the
  scene's own bright pixels. `Renderer::submit(StaticSprites)` and the new submissions share one sprite path.
- **Breaking:** `audio::Music`'s constructor takes the codec (`Music::Codec`). Loading through assets is unchanged.
- The mixer sums each bus separately before applying its gain, so bus effects can work on it.
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
