# Engine modules. Each is a static library with its own public include folder; ph::engine links them all.
#
#   ph_core      no dependencies: types, math, logging, jobs, profiler, files, assets, shader bundles
#   ph_ecs       entities, components, queries, commands, parallel schedules     (core)
#   ph_platform  window and input                                               (core, SDL3)
#   ph_gfx       GPU device, textures, shaders, sprite batching, renderer        (core, ecs, SDL3, stb)
#   ph_audio     mixer, sounds, streamed music                                   (core, SDL3, stb)
#   ph_physics   collision shapes, sweeps, raycasts, spatial hash, move_and_slide, colliders and triggers (core, ecs)
#   ph_ui        immediate-mode game UI: panels, widgets, focus navigation, dialogue (gfx, platform)
#   ph_app       Engine/Game, main loop, dev tools                               (everything)

set(PH_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set(PH_ENGINE_ASSET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE INTERNAL "")
set(PH_ENGINE_ASSET_BUILD_DIR "${CMAKE_BINARY_DIR}/engine_assets" CACHE INTERNAL "")

function(ph_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    add_library(ph_${name} STATIC ${ARG_SOURCES})
    add_library(ph::${name} ALIAS ph_${name})
    target_include_directories(ph_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${name}/include)
    target_link_libraries(ph_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    ph_configure_target(ph_${name})
    ph_optimize_debug(ph_${name})
    set_target_properties(ph_${name} PROPERTIES FOLDER engine)
endfunction()

find_package(Threads REQUIRED)

ph_module(core
    SOURCES
        core/src/assets.cpp
        core/src/core.cpp
        core/src/file.cpp
        core/src/jobs.cpp
        core/src/json.cpp
        core/src/localization.cpp
        core/src/profiler.cpp
        core/src/settings.cpp
    PUBLIC_DEPS Threads::Threads)
if(PH_TRACY)
    target_link_libraries(ph_core PRIVATE Tracy::TracyClient)
    target_compile_definitions(ph_core PRIVATE PH_TRACY=1)
endif()

ph_module(ecs
    SOURCES
        ecs/src/archetype.cpp
        ecs/src/hierarchy.cpp
        ecs/src/schedule.cpp
        ecs/src/world.cpp
        ecs/src/world_io.cpp
    PUBLIC_DEPS ph_core)

ph_module(platform
    SOURCES
        platform/src/input.cpp
        platform/src/window.cpp
    PUBLIC_DEPS ph_core SDL3::SDL3)

ph_module(gfx
    SOURCES
        gfx/src/device.cpp
        gfx/src/loaders.cpp
        gfx/src/particles.cpp
        gfx/src/renderer.cpp
        gfx/src/sprite.cpp
        gfx/src/sprite_sheet.cpp
        gfx/src/sprites.cpp
        gfx/src/text.cpp
        gfx/src/tilemap.cpp
    PUBLIC_DEPS ph_core ph_ecs SDL3::SDL3
    PRIVATE_DEPS ph_stb)
if(PH_DEV_TOOLS)
    target_link_libraries(ph_gfx PRIVATE ph_shaderc)
    target_compile_definitions(ph_gfx PRIVATE PH_RUNTIME_SHADER_COMPILER=1)
endif()
if(WIN32)
    target_link_libraries(ph_gfx PRIVATE d3dcompiler) # HLSL -> DXBC for the D3D12 backend
endif()

ph_module(audio
    SOURCES
        audio/src/audio.cpp
        audio/src/vorbis.cpp
    PUBLIC_DEPS ph_core SDL3::SDL3
    PRIVATE_DEPS ph_stb)

ph_module(physics
    SOURCES
        physics/src/colliders.cpp
        physics/src/collision.cpp
        physics/src/shapes.cpp
    PUBLIC_DEPS ph_core ph_ecs)
if(PH_BOX2D)
    target_sources(ph_physics PRIVATE physics/src/rigid_bodies.cpp)
    target_link_libraries(ph_physics PRIVATE box2d)
    target_compile_definitions(ph_physics PUBLIC PH_BOX2D=1)
endif()

ph_module(ui
    SOURCES ui/src/ui.cpp
    PUBLIC_DEPS ph_gfx ph_platform)

ph_module(app
    SOURCES
        app/src/app.cpp
        app/src/dev_tools.cpp
        app/src/hot_reload.cpp
        app/src/inspector.cpp
        app/src/scene.cpp
    PUBLIC_DEPS ph_core ph_ecs ph_platform ph_gfx ph_audio ph_physics ph_ui)
target_compile_definitions(ph_app PRIVATE PH_ENGINE_ASSET_SOURCE_DIR="${PH_ENGINE_ASSET_SOURCE_DIR}")
if(PH_DEV_TOOLS)
    target_link_libraries(ph_app PRIVATE ph_imgui)
endif()

add_library(ph_engine INTERFACE)
add_library(ph::engine ALIAS ph_engine)
target_link_libraries(ph_engine INTERFACE ph_app)
# Dev builds expose Dear ImGui to games so they can draw their own tool windows in Game::dev_ui.
if(PH_DEV_TOOLS)
    target_link_libraries(ph_engine INTERFACE ph_imgui)
endif()

# Engine shaders, precompiled for builds without the runtime compiler.
ph_compile_shaders(engine_shader_outputs "${PH_ENGINE_ASSET_SOURCE_DIR}" "${PH_ENGINE_ASSET_BUILD_DIR}")
add_custom_target(ph_engine_assets DEPENDS ${engine_shader_outputs})
set_target_properties(ph_engine_assets PROPERTIES FOLDER assets)
