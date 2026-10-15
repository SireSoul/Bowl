# Roadmap

Ordered by what a real game needs first. Each milestone should leave the engine shippable, tested and green on CI.

## M1: make a game with it (content basics)
- ~~Sprite sheets and animation: Aseprite JSON (tags, durations, ping-pong, repeat, trimming), `SpriteAnimator`,
  slices (hitboxes, nine-slice, pivots), animation events by name, frames by name, the `ph-atlas` packer~~ (done)
- ~~Text: TTF fonts, `draw_text`, UTF-8, kerning, wrapping, alignment, outlines and shadows, rich text markup,
  font fallback chains, SDF text~~ (done). Later: color emoji, right-to-left and complex scripts (shaping)
- ~~Tilemaps: Tiled `.tmj`/`.tsj`, orthogonal + isometric, infinite maps, compressed layers, flips, animated tiles,
  objects, properties, y-sorting~~ (done). Still to do: staggered/hex maps, `.tmx` XML, GPU-cached static chunks for
  very large maps
- ~~Audio formats: OGG Vorbis sounds, streamed music with gapless loops and crossfades~~ (done). Still to do: Opus,
  per-bus effects (low-pass, reverb), positional audio helpers
- ~~Collision: shapes, sweeps, raycasts, spatial hash, `move_and_slide` against grids or boxes, polygons and
  capsules, colliders and trigger events as ECS components, optional rigid-body physics (Box2D v3)~~ (done). Later:
  Box2D joints and contact events as components

## M2: worlds that persist
- ~~Reflection: `PH_REFLECT(Type, fields...)`~~ (done). Later: switch to C++26 reflection when compilers ship it
- ~~Serialization: JSON for reflected types, world save/load, prefabs, entity references remapped on load~~ (done).
  Still to do: prefab and scene *assets* (hot-reloaded files), binary format for big worlds, save-file migrations
- ~~Entity inspector: select, edit fields live, duplicate, destroy, copy as JSON, save/load the world~~ (done)
- ~~Relationships: parent/child `Transform2D` hierarchy, cycle-safe reparenting, recursive destroy~~ (done)

## M3: look great
- ~~Post-processing: HDR scene, bloom, exposure/contrast/saturation/tint, ACES tonemap, vignette, UI layer~~ (done).
  Still to do: user post-effect chain, color-grading LUTs, chromatic aberration/film grain
- ~~2D lighting: additive light map, ambient color~~ (done). Still to do: emissive sprites that ignore the light map,
  normal-mapped sprites, shadow casting
- ~~Custom materials: user fragment shaders per batch, extra textures, uniforms, blend modes~~ (done)
- ~~D3D12: HLSL in shader bundles, compiled to DXBC on Windows; CI renders a sample on D3D12 (WARP)~~ (done).
  Later: cache compiled DXBC on disk, DXIL via DXC for Shader Model 6 features
- ~~Particles (CPU, deterministic, reflected settings)~~ and ~~easing + tweens~~ (done). Later: GPU particles (compute)

## M4: iterate faster than anyone
- ~~Game-code hot reload: game library reloaded on rebuild, world kept, rebuilt automatically when a source file is
  saved, on Windows, macOS and Linux~~ (done). Still to do: keeping non-savable state
- ~~Bowl Editor: a separate app for laying out levels (.world files) with art shelf, outline, inspector, groups, undo,
  autosave and the game's own components~~ (done). Next: a Data workspace (every JSON file, edited in place with its
  comments kept), a Paint studio (pixel art painted where it stands), regions and collision shapes, the game's
  components on Windows. Code-first stays primary
- ~~Tracy profiler integration~~ (done, CPU). GPU timelines need timestamp queries in SDL_GPU

Not planned: a scripting language. Game code is C++, and game-code hot reload gives the quick iteration scripting
would otherwise be for.

## M5: everywhere
- ~~Packaging: release builds with static SDL, macOS .app bundles, `cmake --install` layouts~~ (done). Still to do:
  app icons, code signing + notarization, Windows installer, Linux AppImage/Flatpak, Steam Deck checks
- iOS and Android projects (SDL3 already supports them; needs bundle and asset packaging)
- Web build (Emscripten + WebGPU once SDL_GPU supports it)
- Consoles through SDL's console ports (needs developer agreements)

## M6: together
- Networking: reliable UDP transport, lockstep and rollback helpers built on the deterministic ECS
- Deterministic entity ids for entities spawned in parallel systems

## Docs and tooling
- ~~Full documentation: getting started, tutorial (backed by the `tutorial` sample), a guide per system, reference,
  troubleshooting, FAQ, contributing guide~~ (done). Later: generated API reference from the headers
- Inspector: edit `std::vector` and `std::array` fields, add and remove components, filter entities by component

## Known limitations
- Max 256 component types (`PH_ECS_MAX_COMPONENTS` raises it)
- Archetypes are never freed. Fine for games, but worth compacting eventually
- Compute shaders aren't supported by `ph-shaderc` yet
- Game shaders can't `#include` the engine's shader files by path
- Assets load synchronously on the main thread; there's no background loading yet
