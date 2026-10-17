# Engine modules. Each is a static library with its own public include folder; bowl::engine links them all.
#
#   bowl_core      no dependencies: types, math, logging, jobs, profiler, files, assets, shader bundles
#   bowl_ecs       entities, components, queries, commands, parallel schedules     (core)
#   bowl_platform  window and input                                               (core, SDL3)
#   bowl_gfx       GPU device, textures, shaders, sprite batching, renderer        (core, ecs, SDL3, stb)
#   bowl_audio     mixer, sounds, streamed music, sound events                    (core, SDL3, stb)
#   bowl_physics   collision shapes, sweeps, raycasts, spatial hash, move_and_slide, colliders and triggers (core, ecs)
#   bowl_ui        immediate-mode game UI: panels, widgets, focus navigation, dialogue (gfx, platform)
#   bowl_net       UDP sockets, connections with reliable messages, lockstep and rollback   (core)
#   bowl_app       Engine/Game, main loop, dev tools                               (everything)

set(BOWL_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set(BOWL_ENGINE_ASSET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE INTERNAL "")
set(BOWL_ENGINE_ASSET_BUILD_DIR "${CMAKE_BINARY_DIR}/engine_assets" CACHE INTERNAL "")

function(bowl_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    add_library(bowl_${name} STATIC ${ARG_SOURCES})
    add_library(bowl::${name} ALIAS bowl_${name})
    target_include_directories(bowl_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${name}/include)
    target_link_libraries(bowl_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    bowl_configure_target(bowl_${name})
    bowl_optimize_debug(bowl_${name})
    set_target_properties(bowl_${name} PROPERTIES FOLDER engine)
endfunction()

find_package(Threads REQUIRED)

bowl_module(core
    SOURCES
        core/src/archive.cpp
        core/src/assets.cpp
        core/src/core.cpp
        core/src/file.cpp
        core/src/jobs.cpp
        core/src/json.cpp
        core/src/json_text.cpp
        core/src/localization.cpp
        core/src/profiler.cpp
        core/src/settings.cpp
        core/src/xml.cpp
    PUBLIC_DEPS Threads::Threads)
if(BOWL_TRACY)
    target_link_libraries(bowl_core PRIVATE Tracy::TracyClient)
    target_compile_definitions(bowl_core PRIVATE BOWL_TRACY=1)
endif()

bowl_module(ecs
    SOURCES
        ecs/src/archetype.cpp
        ecs/src/hierarchy.cpp
        ecs/src/schedule.cpp
        ecs/src/world.cpp
        ecs/src/world_io.cpp
    PUBLIC_DEPS bowl_core)

bowl_module(platform
    SOURCES
        platform/src/input.cpp
        platform/src/system.cpp
        platform/src/window.cpp
    PUBLIC_DEPS bowl_core SDL3::SDL3)

bowl_module(gfx
    SOURCES
        gfx/src/bidi.cpp
        gfx/src/color_glyphs.cpp
        gfx/src/device.cpp
        gfx/src/gpu_particles.cpp
        gfx/src/loaders.cpp
        gfx/src/particles.cpp
        gfx/src/renderer.cpp
        gfx/src/sprite.cpp
        gfx/src/sprite_sheet.cpp
        gfx/src/sprites.cpp
        gfx/src/stb_image.cpp
        gfx/src/text.cpp
        gfx/src/tilemap.cpp
        gfx/src/truetype.cpp
    PUBLIC_DEPS bowl_core bowl_ecs SDL3::SDL3
    PRIVATE_DEPS bowl_stb)
if(BOWL_DEV_TOOLS)
    target_link_libraries(bowl_gfx PRIVATE bowl_shaderc)
    target_compile_definitions(bowl_gfx PRIVATE BOWL_RUNTIME_SHADER_COMPILER=1)
endif()
# stb_image's JPEG decoder lets int arithmetic wrap on corrupt data; make that defined (see stb_image.cpp).
if(NOT MSVC)
    set_source_files_properties(gfx/src/stb_image.cpp PROPERTIES COMPILE_OPTIONS -fwrapv)
endif()
if(WIN32)
    target_link_libraries(bowl_gfx PRIVATE d3dcompiler) # HLSL -> DXBC for the D3D12 backend
endif()
if(BOWL_HARFBUZZ)
    target_link_libraries(bowl_gfx PRIVATE bowl_harfbuzz)
    target_compile_definitions(bowl_gfx PRIVATE BOWL_HARFBUZZ=1)
else()
    target_compile_definitions(bowl_gfx PRIVATE BOWL_HARFBUZZ=0)
endif()

bowl_module(audio
    SOURCES
        audio/src/audio.cpp
        audio/src/opus.cpp
        audio/src/sound_event.cpp
        audio/src/vorbis.cpp
    PUBLIC_DEPS bowl_core SDL3::SDL3
    PRIVATE_DEPS bowl_stb)
if(BOWL_OPUS)
    target_link_libraries(bowl_audio PRIVATE opusfile)
    target_compile_definitions(bowl_audio PRIVATE BOWL_OPUS=1)
endif()

bowl_module(physics
    SOURCES
        physics/src/colliders.cpp
        physics/src/collision.cpp
        physics/src/shapes.cpp
    PUBLIC_DEPS bowl_core bowl_ecs)
if(BOWL_BOX2D)
    target_sources(bowl_physics PRIVATE physics/src/rigid_bodies.cpp)
    target_link_libraries(bowl_physics PRIVATE box2d)
    target_compile_definitions(bowl_physics PUBLIC BOWL_BOX2D=1)
endif()

bowl_module(ui
    SOURCES ui/src/ui.cpp
    PUBLIC_DEPS bowl_gfx bowl_platform)

bowl_module(net
    SOURCES
        net/src/crypto.cpp
        net/src/host.cpp
        net/src/rendezvous.cpp
        net/src/replication.cpp
        net/src/socket.cpp
    PUBLIC_DEPS bowl_core bowl_ecs
    PRIVATE_DEPS bowl_monocypher)
if(WIN32)
    target_link_libraries(bowl_net PRIVATE ws2_32 bcrypt) # bcrypt: the system's secure random numbers
endif()

bowl_module(app
    SOURCES
        app/src/app.cpp
        app/src/dev_tools.cpp
        app/src/icon.cpp
        app/src/hot_reload.cpp
        app/src/inspector.cpp
        app/src/scene.cpp
    PUBLIC_DEPS bowl_core bowl_ecs bowl_platform bowl_gfx bowl_audio bowl_physics bowl_ui)
target_compile_definitions(bowl_app PRIVATE BOWL_ENGINE_ASSET_SOURCE_DIR="${BOWL_ENGINE_ASSET_SOURCE_DIR}")
if(BOWL_DEV_TOOLS)
    target_link_libraries(bowl_app PRIVATE bowl_imgui)
endif()

add_library(bowl_engine INTERFACE)
add_library(bowl::engine ALIAS bowl_engine)
target_link_libraries(bowl_engine INTERFACE bowl_app bowl_net)
# Dev builds expose Dear ImGui to games so they can draw their own tool windows in Game::dev_ui.
if(BOWL_DEV_TOOLS)
    target_link_libraries(bowl_engine INTERFACE bowl_imgui)
endif()

# Engine shaders, precompiled for builds without the runtime compiler.
bowl_compile_shaders(engine_shader_outputs "${BOWL_ENGINE_ASSET_SOURCE_DIR}" "${BOWL_ENGINE_ASSET_BUILD_DIR}")
add_custom_target(bowl_engine_assets DEPENDS ${engine_shader_outputs})
set_target_properties(bowl_engine_assets PROPERTIES FOLDER assets)
