# API reference

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

Every public header, by module. The [guides](../../README.md#guides) explain how the pieces fit together.

| Header | |
| --- | --- |
| [bowl/app/dev_link.hpp](app.md#bowlappdev_linkhpp) | Game to editor. |
| [bowl/app/engine.hpp](app.md#bowlappenginehpp) | Registers the engine's own components (transforms, names, sprites, maps, colliders...), so saved levels that hold them load by name. |
| [bowl/app/entry.hpp](app.md#bowlappentryhpp) | The engine side of the entry point. |
| [bowl/app/hot_reload.hpp](app.md#bowlapphot_reloadhpp) | Game-code hot reload (dev builds). |
| [bowl/app/icon.hpp](app.md#bowlappiconhpp) | The game's icon as a PNG, put into the executable by bowl_add_game(... ICON icon.png); empty without one. |
| [bowl/app/inspector.hpp](app.md#bowlappinspectorhpp) | Drag-and-drop payload type for asset paths (a UTF-8 string, no terminator). |
| [bowl/app/main.hpp](app.md#bowlappmainhpp) | Include in exactly one .cpp of your game and write BOWL_MAIN(YourGame). |
| [bowl/app/scene.hpp](app.md#bowlappscenehpp) | One screen of a game: title, gameplay, inventory, pause menu... Scenes stack: the top one gets input and updates; scenes below keep drawing if the ones above are transparent (a pause menu over the paused game). |
| [bowl/audio/audio.hpp](audio.md#bowlaudioaudiohpp) | Decoded PCM: interleaved stereo float at the mixer rate. |
| [bowl/audio/sound_event.hpp](audio.md#bowlaudiosound_eventhpp) | A sound the game asks for by what happened ("footstep", "door slam") rather than by file: which clip plays, how loud and at what pitch is picked each time, within limits the event's .sound file sets. |
| [bowl/core/archive.hpp](core.md#bowlcorearchivehpp) | An asset archive (.bowlpak): many files in one, stored as they are, with an index at the front. |
| [bowl/core/assert.hpp](core.md#bowlcoreasserthpp) | BOWL_ASSERT(cond[, "message"]): checked in debug and dev builds, compiled out when BOWL_ASSERTS is 0. |
| [bowl/core/assets.hpp](core.md#bowlcoreassetshpp) | Shared handle to a loaded asset. |
| [bowl/core/ease.hpp](core.md#bowlcoreeasehpp) | Easing curves: t in 0..1 -> 0..1 (some overshoot, like back and elastic). |
| [bowl/core/file.hpp](core.md#bowlcorefilehpp) | Polls modification times. |
| [bowl/core/function_ref.hpp](core.md#bowlcorefunction_refhpp) |  |
| [bowl/core/jobs.hpp](core.md#bowlcorejobshpp) | Counts outstanding jobs. |
| [bowl/core/json.hpp](core.md#bowlcorejsonhpp) | A JSON document node. |
| [bowl/core/json_text.hpp](core.md#bowlcorejson_texthpp) | A JSON document edited as text: each change rewrites only the value it touches, so comments, spacing, key order and trailing commas stay as the person who wrote the file left them. |
| [bowl/core/localization.hpp](core.md#bowlcorelocalizationhpp) | One language's strings, from a JSON file (assets "lang/&lt;code>.json"). |
| [bowl/core/log.hpp](core.md#bowlcoreloghpp) | Sinks receive every line at or above the level, from any thread (calls are serialized). |
| [bowl/core/math.hpp](core.md#bowlcoremathhpp) | Column-major 4x4 matrix (matches GLSL / std140), for column vectors: v' = M * v. |
| [bowl/core/profiler.hpp](core.md#bowlcoreprofilerhpp) | Stats of the last completed frame, sorted by time descending. |
| [bowl/core/reflect.hpp](core.md#bowlcorereflecthpp) | Reflection without code generation. |
| [bowl/core/serialize.hpp](core.md#bowlcoreserializehpp) | State shared across one save or load. |
| [bowl/core/settings.hpp](core.md#bowlcoresettingshpp) | Player preferences that outlive the game: volume, language, window mode, controls. |
| [bowl/core/shader_bundle.hpp](core.md#bowlcoreshader_bundlehpp) | Hlsl is source (Shader Model 5.1, SDL's register layout); on Windows it's compiled to Dxbc when the D3D12 backend runs, since the HLSL compiler ships with Windows but not with other build hosts. |
| [bowl/core/time.hpp](core.md#bowlcoretimehpp) | Seconds since the process started, from a monotonic clock. |
| [bowl/core/type_id.hpp](core.md#bowlcoretype_idhpp) | The compiler's own description of T, from a function outside every namespace: GCC writes a type relative to the namespace of the function it describes, so from inside namespace bowl it wrote "gfx::Map" for bowl::gfx::Map. |
| [bowl/core/types.hpp](core.md#bowlcoretypeshpp) | Non-copyable, non-movable base for objects with identity (devices, worlds, windows). |
| [bowl/core/utf8.hpp](core.md#bowlcoreutf8hpp) | Decodes the code point at s[i] and advances i past it. |
| [bowl/core/xml.hpp](core.md#bowlcorexmlhpp) | One element of an XML document. |
| [bowl/ecs/archetype.hpp](ecs.md#bowlecsarchetypehpp) | All entities with exactly the same set of components live in one archetype, each component type in its own tightly packed array (structure of arrays), so a query walks memory front to back. |
| [bowl/ecs/commands.hpp](ecs.md#bowlecscommandshpp) | Records structural changes (create, destroy, add, remove) to apply later, in recording order. |
| [bowl/ecs/component.hpp](ecs.md#bowlecscomponenthpp) | An entity is an index plus a generation. |
| [bowl/ecs/ecs.hpp](ecs.md#bowlecsecshpp) |  |
| [bowl/ecs/hierarchy.hpp](ecs.md#bowlecshierarchyhpp) | Parent/child transforms. |
| [bowl/ecs/schedule.hpp](ecs.md#bowlecsschedulehpp) | What a schedule's system does and where it runs, for tools (the F1 overlay, Bowl Editor's Game tab). |
| [bowl/ecs/world.hpp](ecs.md#bowlecsworldhpp) | Extra include/exclude terms for a query, for components you filter on but don't read. |
| [bowl/ecs/world_io.hpp](ecs.md#bowlecsworld_iohpp) | Saving and loading entities as JSON, for save games, scenes, prefabs and copy/paste in tools. |
| [bowl/gfx/bidi.hpp](gfx.md#bowlgfxbidihpp) | Embedding levels for one paragraph's characters: even runs left to right, odd right to left. |
| [bowl/gfx/bwtx.hpp](gfx.md#bowlgfxbwtxhpp) | BWTX: the picture format bowl-cook turns PNG and JPEG files into. |
| [bowl/gfx/device.hpp](gfx.md#bowlgfxdevicehpp) | A rectangle of whole pixels in a texture, top-left origin. |
| [bowl/gfx/gpu_particles.hpp](gfx.md#bowlgfxgpu_particleshpp) | How a GpuParticles emitter behaves. |
| [bowl/gfx/material.hpp](gfx.md#bowlgfxmaterialhpp) | How a material's sprites stand in the way of light_material lights. |
| [bowl/gfx/particles.hpp](gfx.md#bowlgfxparticleshpp) | What an emitter spawns. |
| [bowl/gfx/renderer.hpp](gfx.md#bowlgfxrendererhpp) | Whole-screen look. |
| [bowl/gfx/sprite.hpp](gfx.md#bowlgfxspritehpp) | An entity that looks like something: a texture (or a plain rectangle) drawn at the entity's GlobalTransform2D. |
| [bowl/gfx/sprite_sheet.hpp](gfx.md#bowlgfxsprite_sheethpp) | Something that happens on a frame of an animation: a footstep, the moment a swing hits. |
| [bowl/gfx/sprites.hpp](gfx.md#bowlgfxspriteshpp) | GPU layout of one sprite (std430). |
| [bowl/gfx/text.hpp](gfx.md#bowlgfxtexthpp) | A TrueType/OpenType font. |
| [bowl/gfx/tilemap.hpp](gfx.md#bowlgfxtilemaphpp) | Tiled maps (mapeditor.org), loaded from Tiled's JSON (.tmj, .tsj) or XML (.tmx, .tsx) files. |
| [bowl/net/host.hpp](net.md#bowlnethosthpp) | Connections over UDP: the same Host is a server (others connect to it) or a client (it connects to one), or both. |
| [bowl/net/lockstep.hpp](net.md#bowlnetlockstephpp) | One player's inputs for a run of ticks, as Lockstep and Rollback send them. |
| [bowl/net/rendezvous.hpp](net.md#bowlnetrendezvoushpp) | The server players meet through when they can't reach each other directly: hosts behind home routers tell it which room they host (Host::host_room), players ask it for a room (Host::join_room), and it introduces them, so they can connect straight to each other. |
| [bowl/net/replication.hpp](net.md#bowlnetreplicationhpp) | Plain structs as messages. |
| [bowl/net/rollback.hpp](net.md#bowlnetrollbackhpp) | Rollback: nobody waits. |
| [bowl/net/socket.hpp](net.md#bowlnetsockethpp) | An IPv4 or IPv6 address and port. |
| [bowl/physics/colliders.hpp](physics.md#bowlphysicscollidershpp) | A collision shape on an entity, placed by its GlobalTransform2D (position, rotation and scale all apply). |
| [bowl/physics/collision.hpp](physics.md#bowlphysicscollisionhpp) | Collision for games that move things around a world: overlap tests that say how to separate, swept boxes that never tunnel, raycasts, a spatial hash broadphase, and move_and_slide for characters. |
| [bowl/physics/rigid_bodies.hpp](physics.md#bowlphysicsrigid_bodieshpp) | Rigid-body physics through Box2D v3: gravity, stacking, bouncing, things that tip over. |
| [bowl/platform/input.hpp](platform.md#bowlplatforminputhpp) | Values are SDL scancodes (physical key positions: WASD stays WASD on AZERTY keyboards). |
| [bowl/platform/system.hpp](platform.md#bowlplatformsystemhpp) | Whether the game runs on a Steam Deck: Steam says so (the SteamDeck environment variable), or the hardware is one. |
| [bowl/platform/window.hpp](platform.md#bowlplatformwindowhpp) |  |
| [bowl/ui/ui.hpp](ui.md#bowluiuihpp) | Immediate-mode game UI: menus, settings screens, HUD widgets, dialogue. |
