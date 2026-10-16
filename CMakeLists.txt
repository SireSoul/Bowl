cmake_minimum_required(VERSION 3.25)

# Bowl Engine. Code uses the short namespace `bowl`.
project(Bowl VERSION 0.2.0 LANGUAGES C CXX)

option(BOWL_BUILD_TESTS   "Build unit tests and benchmarks"                          ${PROJECT_IS_TOP_LEVEL})
option(BOWL_BUILD_SAMPLES "Build sample games"                                       ${PROJECT_IS_TOP_LEVEL})
option(BOWL_DEV_TOOLS     "Dev build: ImGui overlay, runtime shader compiler, hot reload" ON)
option(BOWL_FETCH_SDL     "Build SDL3 from source even if an installed SDL3 is found" OFF)
option(BOWL_WARNINGS_AS_ERRORS "Treat engine warnings as errors"                     OFF)
option(BOWL_MACOS_BUNDLES "Build macOS games as .app bundles"                          OFF)
option(BOWL_PACK_ASSETS   "Pack each game's engine and asset folders into archives (.bowlpak)" OFF)
option(BOWL_TRACY         "Send profiler zones to the Tracy profiler"                  OFF)
option(BOWL_BOX2D         "Rigid-body physics through Box2D (physics/rigid_bodies.hpp)" OFF)
option(BOWL_OPUS          "Ogg Opus sounds and music (fetches libogg, libopus, opusfile)" OFF)
option(BOWL_HARFBUZZ      "Shape text with HarfBuzz: Arabic, Indic and other complex scripts, ligatures" OFF)
option(BOWL_BUILD_EDITOR "Build Bowl Editor (needs BOWL_DEV_TOOLS)"                   ${PROJECT_IS_TOP_LEVEL})
set(BOWL_SHADERC_EXECUTABLE "" CACHE FILEPATH
    "Prebuilt host bowl-shaderc to use when cross-compiling (iOS, Android, consoles)")
# Cross-compiling: the build runs bowl-shaderc, bowl-icon and bowl-pack, so they come from a build for this machine (the
# host-tools preset puts them in build/host-tools/bin) instead of being built for the target.
set(BOWL_HOST_TOOLS "" CACHE PATH "Folder with bowl-shaderc, bowl-icon and bowl-pack built for this machine, for cross-compiling")
if(BOWL_HOST_TOOLS AND NOT BOWL_SHADERC_EXECUTABLE)
    set(BOWL_SHADERC_EXECUTABLE "${BOWL_HOST_TOOLS}/bowl-shaderc${CMAKE_HOST_EXECUTABLE_SUFFIX}"
        CACHE FILEPATH "" FORCE)
endif()
set(BOWL_TOOLS_DIR "${CMAKE_BINARY_DIR}/bin") # where the tools this build makes go

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(BowlCompiler)
include(BowlDependencies)
include(BowlShaders)
include(BowlPackaging)

add_subdirectory(tools/shaderc)
add_subdirectory(engine)
if(NOT BOWL_HOST_TOOLS) # built for this machine; a cross build uses the host's instead
    add_subdirectory(tools/atlas)
    add_subdirectory(tools/icon)
    add_subdirectory(tools/pack)
    add_subdirectory(tools/apidoc)
    if(NOT IOS)
        add_subdirectory(tools/rendezvous)
    endif()
endif()

if(BOWL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BOWL_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(BOWL_BUILD_EDITOR AND BOWL_DEV_TOOLS)
    add_subdirectory(editor)
endif()
