# PLACEHOLDERENGINENAME vs Halley

Halley (github.com/amzeratul/halley) is a mature engine that shipped Wargroove on PC, Switch, Xbox One and PS4. We're
weeks old. The goal is to beat it everywhere. This page tracks where that's already true, where it isn't yet, and how
we plan to get there. ✅ ahead · 🟰 on par · ❌ behind (planned).

| Area | Halley | Us | |
| --- | --- | --- | --- |
| Language | C++17 | C++20 (concepts, `std::format`, `std::span`, designated initializers) | ✅ |
| Graphics backends | separate plugins: DX11, OpenGL, Metal (experimental) | one renderer on SDL_GPU: Vulkan, Metal, D3D12 | ✅ |
| Shaders | per-backend shader handling | write GLSL once; compiled to SPIR-V + MSL with SDL's binding layout checked | ✅ |
| Live shader editing | asset hot reload | compiled in-process on save; a broken shader keeps the last good one | ✅ |
| ECS | component/system code generated from YAML | plain C++ structs, no codegen; archetype SoA storage; cached queries | ✅ |
| Parallel systems | single-threaded system execution | automatic parallel batches from declared access, plus `par_each` | ✅ |
| Determinism | not a design goal of the ECS | identical data with or without threads (tested); parallel-spawn ids are the one gap | ✅ |
| Fixed timestep | yes | yes, with interpolation alpha and tap-safe input edges per tick | 🟰 |
| Input | SDL input, bindings | actions over keys/mouse/pads/axes, dead zones, rumble | 🟰 |
| Audio | full engine: streaming, Vorbis/Opus, effects, audio objects | mixer: voices, buses, pitch, pan, loop, WAV | ❌ |
| Assets | importer pipeline (atlases, spritesheets, fonts, audio) | mounts, loaders, hot reload (PNG/JPG/TGA/BMP, WAV, GLSL) | ❌ (no importer/atlas yet) |
| Text & fonts | yes | no | ❌ |
| UI library | yes (game UI) | Dear ImGui for tools only | ❌ |
| Scenes, prefabs, serialization | yes (YAML) | no | ❌ |
| Editor | full editor app | in-game overlay: profiler, systems, entities, assets, log, pause/step | ❌ (overlay is ahead of Halley's in-game tools; no editor app) |
| Scripting | Lua | none yet | ❌ |
| Networking | yes (`net`, asio) | none yet | ❌ |
| Code hot reload | experimental runner | none yet | ❌ |
| Platforms | Win, Mac, Linux; consoles (private) | Win, Mac, Linux via CI. SDL3 also targets iOS, Android, web and consoles, but packaging isn't done | 🟰 (consoles need SDL's NDA ports) |
| Entry point | own main loop per platform | SDL3 callbacks, required for web/iOS/Android | ✅ |
| Dependencies | manual setup (`CMAKE_INCLUDE_PATH`...) | `cmake --preset dev`: everything pinned + hash-checked, fetched automatically | ✅ |
| Tests & CI | entity/network stress tests | 40 unit tests, determinism tests, benchmark, 3-OS CI matrix | ✅ |
| Profiling | yes | scoped CPU zones + per-system timings in the overlay | 🟰 |

**Verdict today:** the foundations (language, rendering architecture, shader pipeline, ECS design, threading, build,
iteration speed) are ahead. Content features (text, game UI, scenes, audio formats, editor, scripting, networking) are
behind. Those are the next milestones in [roadmap.md](roadmap.md).
