# Roadmap

Ordered by what a real game needs first. Each milestone should leave the engine shippable, tested and green on CI.

## M1: make a game with it (content basics)
- Texture atlases and sprite sheets: a packer in `tools/`, animated sprites (frame timing, events)
- Text: TTF fonts via SDF glyph atlases, a `draw_text` on the sprite batch, UTF-8, outlines
- Tilemaps: Tiled (`.tmx`) import, chunked rendering, collision layers
- Audio formats: OGG Vorbis/Opus streaming for music, crossfades, per-bus effects (low-pass, reverb)
- Collision/physics: shapes, broadphase grid, overlap queries as ECS systems

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
