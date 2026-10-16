# API reference

Made by `ph-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target ph_api_docs`.

Every public header, by module. The [guides](../../README.md#guides) explain how the pieces fit together.

| Header | |
| --- | --- |
| [ph/app/engine.hpp](app.md#phappenginehpp) | Implement this, then put PH_MAIN(YourGame) in one .cpp file (see ph/app/main.hpp). |
| [ph/app/entry.hpp](app.md#phappentryhpp) | The engine side of the entry point. |
| [ph/app/hot_reload.hpp](app.md#phapphot_reloadhpp) | Game-code hot reload (dev builds). |
| [ph/app/icon.hpp](app.md#phappiconhpp) | The game's icon as a PNG, put into the executable by ph_add_game(... ICON icon.png); empty without one. |
| [ph/app/inspector.hpp](app.md#phappinspectorhpp) | Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). |
| [ph/app/main.hpp](app.md#phappmainhpp) | Include in exactly one .cpp of your game and write PH_MAIN(YourGame). |
| [ph/app/scene.hpp](app.md#phappscenehpp) | One screen of a game: title, gameplay, inventory, pause menu... Scenes stack: the top one gets input and updates; scenes below keep drawing if the ones above are transparent (a pause menu over the paused game). |
| [ph/audio/audio.hpp](audio.md#phaudioaudiohpp) | Decoded PCM: interleaved stereo float at the mixer rate. |
| [ph/core/archive.hpp](core.md#phcorearchivehpp) | An asset archive (.phpak): many files in one, stored as they are, with an index at the front. |
| [ph/core/assert.hpp](core.md#phcoreasserthpp) | PH_ASSERT(cond[, "message"]): checked in debug and dev builds, compiled out when PH_ASSERTS is 0. |
| [ph/core/assets.hpp](core.md#phcoreassetshpp) | Shared handle to a loaded asset. |
| [ph/core/ease.hpp](core.md#phcoreeasehpp) | Easing curves: t in 0..1 -> 0..1 (some overshoot, like back and elastic). |
| [ph/core/file.hpp](core.md#phcorefilehpp) | Polls modification times. |
| [ph/core/function_ref.hpp](core.md#phcorefunction_refhpp) |  |
| [ph/core/jobs.hpp](core.md#phcorejobshpp) | Counts outstanding jobs. |
| [ph/core/json.hpp](core.md#phcorejsonhpp) | A JSON document node. |
| [ph/core/json_text.hpp](core.md#phcorejson_texthpp) | A JSON document edited as text: each change rewrites only the value it touches, so comments, spacing, key order and trailing commas stay as the person who wrote the file left them. |
| [ph/core/localization.hpp](core.md#phcorelocalizationhpp) | One language's strings, from a JSON file (assets "lang/&lt;code>.json"). |
| [ph/core/log.hpp](core.md#phcoreloghpp) | Sinks receive every line at or above the level, from any thread (calls are serialized). |
| [ph/core/math.hpp](core.md#phcoremathhpp) | Column-major 4x4 matrix (matches GLSL / std140), for column vectors: v' = M * v. |
| [ph/core/profiler.hpp](core.md#phcoreprofilerhpp) | Stats of the last completed frame, sorted by time descending. |
| [ph/core/reflect.hpp](core.md#phcorereflecthpp) | Reflection without code generation. |
| [ph/core/serialize.hpp](core.md#phcoreserializehpp) | State shared across one save or load. |
| [ph/core/settings.hpp](core.md#phcoresettingshpp) | Player preferences that outlive the game: volume, language, window mode, controls. |
| [ph/core/shader_bundle.hpp](core.md#phcoreshader_bundlehpp) | Hlsl is source (Shader Model 5.1, SDL's register layout); on Windows it's compiled to Dxbc when the D3D12 backend runs, since the HLSL compiler ships with Windows but not with other build hosts. |
| [ph/core/time.hpp](core.md#phcoretimehpp) | Seconds since the process started, from a monotonic clock. |
| [ph/core/type_id.hpp](core.md#phcoretype_idhpp) | Compile-time type name, e.g. type_name&lt;game::Health>() == "game::Health". |
| [ph/core/types.hpp](core.md#phcoretypeshpp) | Non-copyable, non-movable base for objects with identity (devices, worlds, windows). |
| [ph/core/utf8.hpp](core.md#phcoreutf8hpp) | Decodes the code point at s[i] and advances i past it. |
| [ph/core/xml.hpp](core.md#phcorexmlhpp) | One element of an XML document. |
| [ph/ecs/archetype.hpp](ecs.md#phecsarchetypehpp) | All entities with exactly the same set of components live in one archetype, each component type in its own tightly packed array (structure of arrays), so a query walks memory front to back. |
| [ph/ecs/commands.hpp](ecs.md#phecscommandshpp) | Records structural changes (create, destroy, add, remove) to apply later, in recording order. |
| [ph/ecs/component.hpp](ecs.md#phecscomponenthpp) | An entity is an index plus a generation. |
| [ph/ecs/ecs.hpp](ecs.md#phecsecshpp) |  |
| [ph/ecs/hierarchy.hpp](ecs.md#phecshierarchyhpp) | Parent/child transforms. |
| [ph/ecs/schedule.hpp](ecs.md#phecsschedulehpp) | An ordered list of systems. |
| [ph/ecs/world.hpp](ecs.md#phecsworldhpp) | Extra include/exclude terms for a query, for components you filter on but don't read. |
| [ph/ecs/world_io.hpp](ecs.md#phecsworld_iohpp) | Saving and loading entities as JSON, for save games, scenes, prefabs and copy/paste in tools. |
| [ph/gfx/bidi.hpp](gfx.md#phgfxbidihpp) | Embedding levels for one paragraph's characters: even runs left to right, odd right to left. |
| [ph/gfx/device.hpp](gfx.md#phgfxdevicehpp) | A rectangle of whole pixels in a texture, top-left origin. |
| [ph/gfx/gpu_particles.hpp](gfx.md#phgfxgpu_particleshpp) | How a GpuParticles emitter behaves. |
| [ph/gfx/particles.hpp](gfx.md#phgfxparticleshpp) | What an emitter spawns. |
| [ph/gfx/renderer.hpp](gfx.md#phgfxrendererhpp) | Whole-screen look. |
| [ph/gfx/sprite.hpp](gfx.md#phgfxspritehpp) | An entity that looks like something: a texture (or a plain rectangle) drawn at the entity's GlobalTransform2D. |
| [ph/gfx/sprite_sheet.hpp](gfx.md#phgfxsprite_sheethpp) | Something that happens on a frame of an animation: a footstep, the moment a swing hits. |
| [ph/gfx/sprites.hpp](gfx.md#phgfxspriteshpp) | GPU layout of one sprite (std430). |
| [ph/gfx/text.hpp](gfx.md#phgfxtexthpp) | A TrueType/OpenType font. |
| [ph/gfx/tilemap.hpp](gfx.md#phgfxtilemaphpp) | Tiled maps (mapeditor.org), loaded from Tiled's JSON (.tmj, .tsj) or XML (.tmx, .tsx) files. |
| [ph/net/host.hpp](net.md#phnethosthpp) | Connections over UDP: the same Host is a server (others connect to it) or a client (it connects to one), or both. |
| [ph/net/lockstep.hpp](net.md#phnetlockstephpp) | One player's inputs for a run of ticks, as Lockstep and Rollback send them. |
| [ph/net/rollback.hpp](net.md#phnetrollbackhpp) | Rollback: nobody waits. |
| [ph/net/socket.hpp](net.md#phnetsockethpp) | An IPv4 or IPv6 address and port. |
| [ph/physics/colliders.hpp](physics.md#phphysicscollidershpp) | A collision shape on an entity, placed by its GlobalTransform2D (position, rotation and scale all apply). |
| [ph/physics/collision.hpp](physics.md#phphysicscollisionhpp) | Collision for games that move things around a world: overlap tests that say how to separate, swept boxes that never tunnel, raycasts, a spatial hash broadphase, and move_and_slide for characters. |
| [ph/physics/rigid_bodies.hpp](physics.md#phphysicsrigid_bodieshpp) | Rigid-body physics through Box2D v3: gravity, stacking, bouncing, things that tip over. |
| [ph/platform/input.hpp](platform.md#phplatforminputhpp) | Values are SDL scancodes (physical key positions: WASD stays WASD on AZERTY keyboards). |
| [ph/platform/system.hpp](platform.md#phplatformsystemhpp) | Whether the game runs on a Steam Deck: Steam says so (the SteamDeck environment variable), or the hardware is one. |
| [ph/platform/window.hpp](platform.md#phplatformwindowhpp) |  |
| [ph/ui/ui.hpp](ui.md#phuiuihpp) | Immediate-mode game UI: menus, settings screens, HUD widgets, dialogue. |
