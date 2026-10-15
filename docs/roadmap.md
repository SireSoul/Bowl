# Roadmap

Ordered by what a real game needs first. Each milestone should leave the engine shippable, tested and green on CI.

## M1: make a game with it (content basics)
- ~~Sprite sheets and animation: Aseprite JSON (tags, durations, ping-pong, repeat, trimming), `SpriteAnimator`,
  slices (hitboxes, nine-slice, pivots), animation events by name, frames by name, the `ph-atlas` packer~~ (done)
- ~~Text: TTF fonts, `draw_text`, UTF-8, kerning, wrapping, alignment, outlines and shadows, rich text markup,
  font fallback chains, SDF text, color emoji~~ (done). Later: right-to-left and complex scripts (shaping)
- ~~Tilemaps: Tiled `.tmj`/`.tsj` and `.tmx`/`.tsx`, orthogonal, isometric, staggered and hexagonal, infinite
  maps, compressed layers, flips, animated tiles, objects, properties, y-sorting, GPU-cached chunks for very large
  maps, object templates, Wang sets as data~~ (done)
- ~~Audio: OGG Vorbis and Opus sounds, streamed music with gapless loops and crossfades, per-bus low-pass, high-pass
  and reverb, positional sounds that follow a listener, per-voice filters, ducking music under dialogue~~ (done)
- ~~Collision: shapes, sweeps, raycasts, spatial hash, `move_and_slide` against grids or boxes, polygons and
  capsules, colliders and trigger events as ECS components, optional rigid-body physics (Box2D v3) with joints
  and contact events~~ (done)

## M2: worlds that persist
- ~~Reflection: `PH_REFLECT(Type, fields...)`~~ (done). Later: switch to C++26 reflection when compilers ship it
- ~~Serialization: JSON for reflected types, world save/load, prefabs, entity references remapped on load, prefab and
  world files as hot-reloading assets, a binary form for big worlds, save-file migrations~~ (done)
- ~~Entity inspector: select, edit fields live, duplicate, destroy, copy as JSON, save/load the world~~ (done)
- ~~Relationships: parent/child `Transform2D` hierarchy, cycle-safe reparenting, recursive destroy~~ (done)

## M3: look great
- ~~Post-processing: HDR scene, bloom, exposure/contrast/saturation/tint, ACES tonemap, vignette, UI layer, your own
  effect chain, color-grading LUTs, chromatic aberration, film grain~~ (done)
- ~~2D lighting: additive light map, ambient color, emissive sprites, normal-mapped sprites, shadow casting~~ (done)
- ~~Custom materials: user fragment shaders per batch, extra textures, uniforms, blend modes~~ (done)
- ~~D3D12: HLSL in shader bundles, compiled to DXBC on Windows and cached on disk; CI renders a sample on D3D12
  (WARP)~~ (done). Later: DXIL via DXC for Shader Model 6 features
- ~~Particles (CPU, deterministic, reflected settings), GPU particles, compute shaders~~ and ~~easing + tweens~~ (done)

## M4: iterate faster than anyone
- ~~Game-code hot reload: game library reloaded on rebuild, world kept, rebuilt automatically when a source file is
  saved, on Windows, macOS and Linux, keeping the world in place (entity ids and plain-data components survive)~~
  (done)
- ~~Bowl Editor: a separate app for laying out levels (.world files) with art shelf, outline, inspector, groups, undo,
  autosave, the game's own components (on Windows too), a Data workspace (every JSON file, edited in place with its
  comments kept), a Paint workspace and a Brush that paints art where it stands, regions and collision shapes~~ (done).
  Code-first stays primary
- ~~Tracy profiler integration~~ (done, CPU). GPU timelines need timestamp queries in SDL_GPU

Not planned: a scripting language. Game code is C++, and game-code hot reload gives the quick iteration scripting
would otherwise be for.

## M5: everywhere
- ~~Packaging: release builds with static SDL, macOS .app bundles, `cmake --install` layouts, app icons, code signing
  and notarization, installers (DMG, NSIS), Linux AppImage and Flatpak, Steam Deck checks, asset archives~~ (done)
- iOS and Android projects (SDL3 already supports them; needs bundle and asset packaging)
- Web build (Emscripten + WebGPU once SDL_GPU supports it)
- Consoles through SDL's console ports (needs developer agreements)

## M6: together
- ~~Networking: reliable UDP transport, lockstep and rollback helpers built on the deterministic ECS~~ (done). Later:
  encryption, NAT punch-through and relays, state replication
- ~~Deterministic entity ids for entities spawned in parallel systems~~ (done)

## Docs and tooling
- ~~Full documentation: getting started, tutorial (backed by the `tutorial` sample), a guide per system, reference,
  troubleshooting, FAQ, contributing guide, an API reference generated from the headers~~ (done)
- ~~Inspector: edit `std::vector` and `std::array` fields, add and remove components, filter entities~~ (done)

## Known limitations
- Max 256 component types (`PH_ECS_MAX_COMPONENTS` raises it)
