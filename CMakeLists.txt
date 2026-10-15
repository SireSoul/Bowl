cmake_minimum_required(VERSION 3.25)

# Bowl Engine. Code uses the short namespace `ph`.
project(Bowl VERSION 0.1.0 LANGUAGES C CXX)

option(PH_BUILD_TESTS   "Build unit tests and benchmarks"                          ${PROJECT_IS_TOP_LEVEL})
option(PH_BUILD_SAMPLES "Build sample games"                                       ${PROJECT_IS_TOP_LEVEL})
option(PH_DEV_TOOLS     "Dev build: ImGui overlay, runtime shader compiler, hot reload" ON)
option(PH_FETCH_SDL     "Build SDL3 from source even if an installed SDL3 is found" OFF)
option(PH_WARNINGS_AS_ERRORS "Treat engine warnings as errors"                     OFF)
option(PH_MACOS_BUNDLES "Build macOS games as .app bundles"                          OFF)
option(PH_TRACY         "Send profiler zones to the Tracy profiler"                  OFF)
option(PH_BUILD_EDITOR  "Build Bowl Editor (needs PH_DEV_TOOLS)"                   ${PROJECT_IS_TOP_LEVEL})
set(PH_SHADERC_EXECUTABLE "" CACHE FILEPATH
    "Prebuilt host ph-shaderc to use when cross-compiling (iOS, Android, consoles)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PhCompiler)
include(PhDependencies)
include(PhShaders)

add_subdirectory(tools/shaderc)
add_subdirectory(engine)

if(PH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PH_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(PH_BUILD_EDITOR AND PH_DEV_TOOLS)
    add_subdirectory(editor)
endif()
