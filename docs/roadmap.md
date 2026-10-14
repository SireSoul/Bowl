# Roadmap

Ordered by what a real game needs first. Each milestone should leave the engine shippable, tested and green on CI.

## M1: make a game with it (content basics)
- ~~Sprite sheets and animation: Aseprite JSON (tags, durations, ping-pong, repeat, trimming), `SpriteAnimator`~~
  (done). Still to do: a texture atlas packer in `tools/`, Aseprite slices (hitboxes, 9-slice), animation events by name
- ~~Text: TTF fonts, `draw_text`, UTF-8, kerning, wrapping, alignment~~ (done). Still to do: outlines/shadows, SDF for
  smoothly scaling text, font fallback chains for CJK/emoji, rich text markup
- ~~Tilemaps: Tiled `.tmj`/`.tsj`, orthogonal + isometric, infinite maps, compressed layers, flips, animated tiles,
  objects, properties, y-sorting~~ (done). Still to do: staggered/hex maps, `.tmx` XML, GPU-cached static chunks for
  very large maps
- ~~Audio formats: OGG Vorbis sounds, streamed music with gapless loops and crossfades~~ (done). Still to do: Opus,
  per-bus effects (low-pass, reverb), positional audio helpers
- ~~Collision: shapes, sweeps, raycasts, spatial hash, `move_and_slide` against grids or boxes~~ (done). Still to do:
  polygons/capsules, trigger events as ECS components, optional rigid-body physics (Box2D v3) for games that want it

## M2: worlds that persist
- Reflection for components (a small `PH_REFLECT` macro or C++26 reflection when compilers ship it)
- Serialization: save/load worlds, scene and prefab files (JSON), entity references remapped on load
- Entity inspector: edit component fields live in the overlay
- Relationships: parent/child transforms

## M3: look great
- Post-processing chain on the off-screen scene target: bloom, color grading, vignette
- 2D lighting: light map with normal-mapped sprites
- Custom materials: user shaders per sprite batch, with a uniform block
- D3D12 shader path (SPIRV-Cross -> HLSL -> DXBC/DXIL) so Windows can fall back from Vulkan
- Particles on the GPU (compute)

## M4: iterate faster than anyone
- Game-code hot reload: game logic as a shared library reloaded on rebuild, with world state kept
- Scripting (Lua or Luau) bound to the ECS
- Editor app built on the engine itself (scene view, inspector, asset browser), still optional: code-first stays primary
- Tracy profiler integration (GPU + CPU timelines)

## M5: everywhere
- Packaging: macOS .app, Windows installer, Linux AppImage/Flatpak, Steam Deck checks
- iOS and Android projects (SDL3 already supports them; needs bundle and asset packaging)
- Web build (Emscripten + WebGPU once SDL_GPU supports it)
- Consoles through SDL's console ports (needs developer agreements)

## M6: together
- Networking: reliable UDP transport, lockstep and rollback helpers built on the deterministic ECS
- Deterministic entity ids for entities spawned in parallel systems

## Known limitations
- Max 256 component types (`PH_ECS_MAX_COMPONENTS` raises it)
- Archetypes are never freed. Fine for games, but worth compacting eventually
- Compute shaders aren't supported by `ph-shaderc` yet
