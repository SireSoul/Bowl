# Changelog

Notable changes to the engine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Until
1.0, any release may change the API; changes that break existing games are marked **Breaking**.

## [Unreleased]

### Added
- Sound events (`audio::SoundEvent`, loaded from `.sound` JSON files that hot-reload): weighted clips that never repeat
  the last one, gain, pitch and delay ranges, a bus, looping, filters and attenuation, `switch` lists per value (a
  surface under footsteps), `layers` that play together, and `max_instances` with `steal` and `cooldown`. Play one
  with `AudioEngine::play_event`, which returns an `EventInstance` to stop or check; choices come from the engine's
  own random numbers (`seed_random`). The F1 overlay lists them with play and stop and what each last picked. The
  tutorial's footsteps use one.
- Tiled layers with the bool property `above` draw over everything y-sorted, for what characters walk under (roofs,
  the tops of arches and gates), like Stardew Valley's Front layer. `TilemapCache` leaves them to `draw_tilemap` with
  `Layers::Sorted`.
- `gfx::Map`, a Tiled map placed in the world at an entity's position, and `gfx::draw_maps` to draw every one, sorted
  with the sprites; `draw_sprites` and `draw_maps` take an optional `skip` filter.
- Bowl Editor: Tiled maps (`.tmj`, `.tmx`) and prefabs on the shelf. A map goes in the level as a `gfx::Map`, locked,
  and snapping follows its cells (diamonds on isometric maps) with the cell under the mouse outlined; a prefab places a
  copy of its entities. Hiding and locking things in the editor (an eye and a padlock in the Outline, saved under
  `"editor"`, which games ignore).
- Replication for host-run games (`net::Replicator` on the host, `net::Replica` on players, `net::Replicated`,
  `net::Sync`): entities and their registered components reach each player, `OnChange` ones reliably when they change,
  `Continuous` ones unreliably with the newest winning; entity references are remapped, and `relevant()` picks what each
  player is shown.
- Typed messages (`net::Messages`): reflected structs sent and handled by type, the same across compilers.
- Lobbies: `RoomInfo` (listed rooms with a label and player counts), `Host::list_rooms` with the `RoomsListed` event,
  and `Host::quick_match` into the fullest listed room with space.
- Fuzzers for everything that reads data a stranger could control (`tests/fuzz`, `BOWL_FUZZ`, the `fuzz` preset):
  network packets for `net::Host` and `net::Rendezvous`, `Address::read`, JSON text and binary, `json::TextDocument`,
  saved worlds and prefabs, XML, Tiled maps, Aseprite sheets, asset archives, fonts and pictures. libFuzzer with
  AddressSanitizer and UBSan; `tests/fuzz/run.sh` runs them, and CI runs each for 20 seconds on Linux. See
  [CONTRIBUTING](CONTRIBUTING.md#fuzzing).

### Changed
- Bowl Editor: the Outline's eye hides a thing in the editor only; it used to switch the picture's `Shown`, which the
  game sees. The Art card's **Shown** still does that.
- Fonts are checked before stb_truetype reads them (it checks nothing itself): a font whose tables run past the end of
  the file, or whose metrics can't be right, fails to load, and a glyph whose outline would make stb read outside the
  font, recurse forever or work for seconds draws as nothing. Character lookup and kerning are read with bounds checks.
- Textures, window icons and other pictures are limited to 16384 pixels a side.
- Tile layers are limited to `gfx::max_layer_cells` (16M cells, 4096 x 4096), and a layer whose data holds fewer cells
  than its size (or zlib/gzip data that inflates to more) fails to load instead of being padded with empty cells.

### Fixed
- Y-sorted tiles sort by the middle of their cell (`Tilemap::sort_y`) instead of the bottom of the picture, so a
  character walking up to a tall tile on an isometric map's near sides (down-left, down-right) is drawn in front of it,
  not behind.
- The dev overlay's profiler table no longer flickers: it shows each zone's average per frame over half a second, in
  an order that holds between updates, and keeps zones that don't run every frame (the fixed tick, asset polling)
  instead of dropping them on frames they skip.
- The log's window line shows the size the window got (full screen on a phone, for example), not the size asked
  for.
- Infinite Tiled maps with chunks at positions near the edge of 32-bit range wrote outside the layer's memory.
- A Tiled layer claiming a huge size was allocated before its data was read (a small map file could ask for
  gigabytes); so was each level of a nested binary JSON document, and a binary document could copy one long string
  over and over. All now fail to load instead.
- A font with an empty or too short file, a composite glyph made of itself, a contour starting off the curve on a
  glyph's last point (Times on macOS has one), or a glyph box with its corners swapped made stb_truetype read out of
  bounds, run out of stack or allocate a negative size.
- Numbers too big for the integer they're read into were undefined behavior: `json::Value::as_int` now clamps (NaN gives
  the fallback), saved fields are clamped to their type, and tile gids, Aseprite frame numbers and far-off
  `Tilemap::cell_at` points no longer overflow.
- A BMP 0 pixels high decoded as an empty picture, and a header claiming a huge picture made stb allocate gigabytes
  from a few bytes; JPEGs with corrupt coefficients overflowed `int` in stb_image's decoder.
- `json::TextDocument::erase` left invalid JSON for an item on the opening bracket's line or one whose comma starts the
  next line.

## [0.2.0] - 2026-10-16

### Added
- Networking rooms (`Host::host_room`, `join_room`, `net::Route`): players meet through a rendezvous server
  (`net::Rendezvous`, the `bowl-rendezvous` tool), connect straight to each other through home routers (NAT
  punch-through), or through the server's relay when they can't (`PeerStats::relayed`; `Route::Relay` hides players'
  addresses from each other). `HostSettings::simulated_unreachable` tests the relay on one machine. `Address::write`
  and `Address::read` for addresses in packets.
- iOS and iPadOS: the `ios` preset (Xcode project, iOS 17+; `ios-simulator` and `ios-device` build presets) and
  `host-tools`; `bowl_add_game` makes app bundles with an Info.plist, an asset-catalog icon and packed assets
  (`ORIENTATION`, `BOWL_IOS_TEAM`). `BOWL_HOST_TOOLS` for cross builds. `Input::touches()` and `Device::Touch`,
  `Window::safe_area()`, and `Engine::suspended()`: games stop updating and drawing in the background.
- Right-to-left text: paragraphs mixing Hebrew or Arabic with left-to-right text are drawn in reading order, with
  mirrored brackets (`bowl/gfx/bidi.hpp`, `TextStyle::direction`); typewriter reveals follow reading order.
- Text shaping with HarfBuzz (`BOWL_HARFBUZZ`, `gfx::text_shaping()`): joined Arabic, Devanagari and other complex
  scripts, ligatures and OpenType kerning. `PlacedGlyph::glyph_index`.
- Color emoji: fonts with `sbix`, `CBDT`/`CBLC` or `COLR`/`CPAL` (v0) tables draw their characters in color
  (`Font::Glyph::color`, `PlacedGlyph::color_glyph`); `Font::page_pixels`.
- API reference: `docs/reference/api`, one page per module made from the headers' comments by `bowl-apidoc`
  (`bowl_api_docs` target).
- Asset archives: `bowl::Archive`, `write_archive`, `AssetManager::mount_archive`, `read` and `exists`; the `bowl-pack`
  tool; `BOWL_PACK_ASSETS` (on in the `release` preset) ships `engine.bowlpak` and `assets.bowlpak`, which the App mounts
  when they're next to the game.
- Packaging: `bowl_add_game(... NAME <name> ICON <png> [ICON_PIXEL_ART])` makes the `.icns`, the `.ico` (built into the
  `.exe`), Linux PNG and desktop files, and the window icon (`bowl::embedded_icon`, `Window::set_icon`); the `bowl-icon`
  tool. Installing signs macOS bundles (`BOWL_MACOS_SIGN_IDENTITY`, ad hoc by default; `BOWL_MACOS_ENTITLEMENTS`) and,
  with `BOWL_WINDOWS_SIGN_COMMAND`, Windows executables; `cmake/BowlNotarize.cmake` notarizes and staples. `bowl_package`
  makes installers with `cpack` (DMG, NSIS, ZIP, TGZ), an AppImage (`<game>_appimage`) and a Flatpak manifest
  (`BOWL_LINUX_SHARE`).
- Every game made with `bowl_add_game` carries Bowl's license: `bowl-LICENSE.txt` and `bowl-NOTICE.txt` next to the
  executable (in `Resources/` of a macOS bundle, at the top of an iOS app), loose when assets are packed, and in
  installs, `cpack` installers, AppImages and Flatpaks (also `share/licenses/<bundle id>/` with `BOWL_LINUX_SHARE`).
- Steam Deck: `bowl::is_steam_deck()`, fullscreen there by default (`AppConfig::fullscreen_on_steam_deck`), and
  `--deck` to preview a game in a 1280 x 800 window. `Input::last_device()` for keyboard or gamepad prompts.
- `gfx::decode_png`.
- Networking (`bowl_net`, guide in docs/guides/networking.md): `net::Host`, connections over UDP with reliable ordered
  messages (resent until acknowledged, up to 64 KB) and unreliable ones, timeouts, stats and a simulated bad network;
  `net::Lockstep` and `net::Rollback` (guessed inputs, saved states, re-running ticks when a guess was wrong).
- Tilemaps: Tiled object templates (`.tj`, `.tx`) are filled in when maps load, tile templates mapped onto the
  map's tilesets; terrain sets as data (`WangSet`, `Tileset::wang_sets`, `corner_tile`, `fill_from_corners`).
- Box2D: `physics::Joint` (distance and rope, revolute, prismatic, weld, wheel; springs, limits, motors) and the
  `ContactEvents` resource (touches began and ended, impacts with point and speed; `PhysicsSettings::hit_speed`).
- Audio: per-voice filters (`PlayParams::low_pass`, `high_pass`, `set_filters`) and ducking (`set_ducking`, `Ducking`,
  `duck_level`).
- Background asset loading: `AssetManager::load_async`, `loading`, `finish_loads` (the App calls it every frame) and
  `register_decoder`. Textures and sounds decode on the loading thread; other types read their file there.
- `ecs::World::compact` frees archetypes no entity uses; queries notice (`World::layout_epoch`) and refresh.
- Game shaders can `#include "engine/shaders/common.glsl"`: bowl-shaderc takes include roots (`--include
  <prefix>=<folder>`, `CompileOptions::include_roots`), and builds and dev reloads pass the engine's.
- Bowl Editor: Level, Data and Paint workspaces. Data edits every JSON file in the assets folder as a tree and as text,
  keeping comments and layout; Paint is a pixel-art canvas (pencil, eraser, fill, picker, palette, undo); the Brush
  tool paints on art where it stands in the level; the Region tool and handles draw and resize trigger regions and
  colliders, which are all shown on the level. `--game` works on Windows through `<game>_editor_code.dll`.
- `json::TextDocument`: edits JSON text in place (set, insert, erase) without disturbing comments or layout.
- Inspector: `std::vector` and `std::array` fields are editable (`FieldVisitor::begin_list`), components can be removed
  and added (`inspector::add_component_menu`), and the overlay's entity list has a filter (`inspector::matches`).
- Compute shaders: `bowl-shaderc` compiles `.comp.glsl` for every backend in SDL_GPU's compute layout;
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
  `set_position`, `set_listener`, `Attenuation`, `spatialize`), and Ogg Opus sounds and music with `-DBOWL_OPUS=ON`.
- Tilemaps: Tiled XML maps and tilesets (`.tmx`, `.tsx`, `Tilemap::parse_tmx`), staggered and hexagonal maps
  (`Tilemap::cell_at`, `pixel_size`), and `TilemapCache`, which keeps the flat layers of big maps in GPU memory as
  chunks. `TilemapDrawOptions::layers` draws only the flat or the y-sorted layers.
- `gfx::StaticSprites` and `Renderer::submit(StaticSprites, camera)`: sprites uploaded once and drawn every frame
  without being sent again. `gfx::sprite_order`.
- `bowl/core/xml.hpp`: a small XML reader.
- Text: outlines and shadows (`TextStyle::outline`, `shadow`), rich text markup (`[color=#hex]`, `[wave]`, `[shake]`),
  fallback fonts (`Font::set_fallbacks`, `has_glyph`), distance-field text (`TextStyle::sdf`, `sdf_text_material`), and
  `layout_text` for drawing laid-out characters yourself. `Font::advance` measures without rasterizing.
- Sprite sheets: Aseprite slices (`SpriteSlice`, `slice_rect`, `draw_nine_slice` from a slice), animation events from
  tag user data (`SpriteAnimator::fired`), frame names (`frame_index`). `SpriteBatch::draw_nine_slice` from part of a
  texture.
- `bowl-atlas`: packs a folder of PNGs into an atlas with sprite-sheet JSON (trimming, padding, numbered files become
  animations). The packer is the `bowl_atlas` library.
- Collision: `Capsule` and convex `Polygon` shapes, and `Shape` (any of the four) with `collide`, `overlaps`,
  `contains`, `raycast` and `bounds` for every pair.
- `physics::Collider` component, `update_triggers` and the `TriggerEvents` resource (entered, inside, exited, with
  layers), and `overlapping` queries. `bowl_physics` now depends on `bowl_ecs`.
- Optional Box2D v3 rigid bodies (`-DBOWL_BOX2D=ON`): `RigidBody`, `PhysicsSettings`, `step_physics`, `apply_impulse`.
- Bowl Editor (`bowl-editor`): lays out levels in `.world` files. Art shelf with thumbnails, place and select tools,
  box select, groups, align, copy and paste, undo and redo, autosave, an inspector with cards, and the game's own
  components through `--game`. Unknown components are kept as JSON so saving never loses data.
- `gfx::Sprite`, `gfx::draw_sprites` and `gfx::sprite_bounds`: art on an entity, drawn at its `GlobalTransform2D`.
- `ecs::WorldFile`: `.world` files as hot-reloading assets. `ecs::Name`: a readable name for an entity.
- `World::remove_component` by id. Asset fields are editable through reflection (`FieldVisitor::asset`).
- `bowl/app/inspector.hpp`: the overlay's component editor, shared with tools. `AppConfig::dev_ui_always` for tools
  built on the engine (their overlay key moves to F12).
- Game-code hot reload on Windows; CI runs a real reload there on every push.
- Game-code hot reload rebuilds the code by itself when a source file is saved; compile errors go to the log and the
  previous code keeps running. `--no-auto-build` turns it off. `AutoBuilder` does the watching and building.
- Tracy support (`-DBOWL_TRACY=ON`): profiler zones with source locations, frame marks, worker thread names, log
  lines and plots show up in the Tracy profiler. `profiler::plot` and `profiler::set_thread_name`.
- `tutorial` sample: Fireflies, the game `docs/tutorial.md` builds step by step. CI compiles it, so the tutorial stays
  correct.
- `update_particle_emitters` and `draw_particle_emitters` run every `gfx::ParticleEmitter` in a world. The component
  is now registered with `BOWL_COMPONENT`.
- `GpuDevice::update_texture_region` uploads one rectangle of a texture.
- `Input::unbind_all`.
- `Vec2::distance`, `Vec2::angle`, `Rect::from_center`, `Rect::expanded`.
- Documentation: getting started, a tutorial, a guide per system, CMake and command-line reference, conventions,
  troubleshooting, FAQ, contributing guide, issue and pull request templates.

### Changed
- **Breaking**: `net::Host` connections are encrypted and authenticated (X25519 key agreement, XChaCha20-Poly1305
  packets, with Monocypher), with replays turned away; `HostSettings::secret` keeps out hosts that don't know it.
  Builds before this one can't connect to builds after it. Connection salts come from the system's secure random
  numbers.
- The isometric sample has installers (`bowl_package`), which the Package workflow builds and runs on every
  platform. CI dependency downloads retry when they stall, and jobs stop after 45 minutes.
- Bowl is under the Apache License 2.0 (`LICENSE`, `NOTICE`).
- **Breaking**: the `ph` prefix is now `bowl` everywhere: the `bowl` namespace and `bowl/...` include paths,
  `BOWL_` macros and CMake options, `bowl_add_game` and the other CMake functions, the `bowl-shaderc`, `bowl-atlas`,
  `bowl-icon`, `bowl-pack` and `bowl-apidoc` tools, and the `.bowlpak` and `.bowlsh` file extensions. Archives, binary
  documents and shader bundles start with new magic numbers, so rebuild them.
- The tools a build makes (`bowl-shaderc`, `bowl-icon`, `bowl-pack`, `bowl-atlas`, `bowl-apidoc`) are in its `bin` folder.
- Systems' command buffers hand out stand-in ids (`Entity::is_placeholder`) that become real entities, in system and
  chunk order, when applied: entity ids are now the same every run whatever the thread count. Stand-ins in the
  buffer's commands and in reflected components' entity fields are swapped for the real ids.
  `reflect::FieldVisitor::entity_ref` lets a visitor change entity references.
- **Breaking:** game-code hot reload keeps the world instead of saving and reloading it. Entity ids no longer change,
  and plain-data components survive whether or not they're reflected; savable components that aren't plain data are
  saved and loaded back as before. The engine registers its own component types at startup.
- `ecs::World::forget_code` drops hooks, resources and deferred commands but keeps entities. Dormant component types
  keep their layout, and a type that comes back with a different layout gets a new id.
- `.bowlsh` bundles are format version 2 (compute fields); version 1 bundles still load.
- Lighting is applied in its own pass before bloom (so scene effects see the lit image); bloom still comes from the
  scene's own bright pixels.
- **Breaking:** `audio::Music`'s constructor takes the codec (`Music::Codec`). Loading through assets is unchanged.
- The mixer sums each bus separately before applying its gain, so bus effects can work on it.
- Font atlases upload only the rectangle holding newly added glyphs, instead of the whole 4 MB page.
- `bowl_gfx` depends on `bowl_ecs` (for the particle emitter helpers).
- Every engine target carries C++20 as a usage requirement, so games that add the engine with `add_subdirectory`
  compile its headers correctly without setting `CMAKE_CXX_STANDARD` themselves.
- `Settings::save` is no longer `const`; `Settings::slot` marks settings as changed.
- Comments throughout the code were rewritten to say plainly what the code does, and outdated ones corrected.

### Fixed
- Installers, disk images and AppImages from `bowl_package` carried every game the project installs; now only the
  packaged game (each game installs into a component named after it). The Flatpak manifest skips `build/` and `.git`
  and builds out of the source folder.
- Fonts made only of pictures (no outlines), such as Noto Color Emoji, load.
- Class-type properties in `.tmx` maps no longer read freed memory.
- Game-code hot reload on Windows: each loaded copy of the game library gets its own `.pdb`, so a profiler or
  debugger holding the previous one no longer stops the next build from linking (LNK1201).
- Engine components (sprites, animators, colliders) registered first by a game library no longer count as the
  library's, so they survive game-code hot reload.
- Game-code hot reload no longer duplicates input bindings each time `init` runs again; a changed default binding
  now replaces the old one.
- The `bowl_tests` executable no longer links every module twice (a macOS linker warning).

## 0.1.0 - 2026-10-14

The engine before its first public release; there is no download for it.

The first version: everything needed to start making a game.

### Added
- **Core:** math, easing and tweens, logging, assertions, a job system with deterministic chunking, a CPU profiler,
  file watching, JSON with comments and trailing commas, UTF-8, reflection (`BOWL_REFLECT`), JSON serialization, player
  settings, localization with fallbacks and plurals.
- **Assets:** virtual paths and mounts, shared `Asset<T>` handles, hot reload that keeps the last good version on
  failure, custom loaders.
- **ECS:** archetype storage, cached queries, `par_each`, command buffers, hooks, resources, a parallel scheduler with
  deterministic results, world save and load, prefabs, entity references remapped on load, parent/child transforms.
- **Rendering:** SDL_GPU on Vulkan, Metal and Direct3D 12; instanced sprite batches with one upload per frame;
  cameras; depth and texture sorting; blend modes; custom materials; HDR, bloom, color grading, ACES tonemapping,
  vignette; 2D lighting; a UI layer; screenshots.
- **Shaders:** `bowl-shaderc` compiles GLSL to SPIR-V, MSL and HLSL bundles; dev builds compile and reload shaders while
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

[Unreleased]: https://github.com/SireSoul/Bowl/compare/v0.2.0...develop
[0.2.0]: https://github.com/SireSoul/Bowl/releases/tag/v0.2.0
