# Third-party code. Everything is pinned to a release tarball and its SHA-256, so every machine and CI runner builds
# exactly the same bytes. An installed SDL3 is used when available (faster first build); everything else is fetched.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)
# Some dependencies still declare ancient cmake_minimum_required versions that CMake 4 refuses.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# SYSTEM: third-party headers never trip our warning flags (-Wshadow etc. with -Werror on CI).
function(ph_fetch name url sha256)
    FetchContent_Declare(${name} URL ${url} URL_HASH SHA256=${sha256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM ${ARGN})
endfunction()

# SDL3: window, input, audio, GPU (Vulkan / Metal / D3D12), filesystem, threads
if(NOT PH_FETCH_SDL)
    find_package(SDL3 3.2 CONFIG QUIET)
endif()
if(SDL3_FOUND)
    message(STATUS "Bowl: using installed SDL3 ${SDL3_VERSION}")
else()
    message(STATUS "Bowl: building SDL3 from source")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    ph_fetch(SDL3 https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
             c2ee715e42ec520c4d11fd8d249ef1d2b2baf4ad31148b72b3b000276c0b3633)
    FetchContent_MakeAvailable(SDL3)
endif()

# stb: image decoding (PNG/JPG/TGA/BMP), PNG writing, TrueType, Vorbis
ph_fetch(stb https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
         9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515)
FetchContent_MakeAvailable(stb)
add_library(ph_stb INTERFACE)
target_include_directories(ph_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# Shader compiler: glslang (GLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> MSL, HLSL)
# Needed for the build-time ph-shaderc tool, and linked into dev builds for live shader reload.
set(PH_NEED_SHADER_COMPILER OFF)
if(PH_DEV_TOOLS OR NOT PH_SHADERC_EXECUTABLE)
    set(PH_NEED_SHADER_COMPILER ON)
endif()

if(PH_NEED_SHADER_COMPILER)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    ph_fetch(glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/16.5.0.tar.gz
             01af17195fbeb59e39e31e9506de35bb39dfd35807ea0c9a1a99d7d1183ddd45)

    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    ph_fetch(spirv_cross https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
             b2665ac9ddb65ff75a7ca2f6d410e73da443692742a1e3a5b7728ca6069a400c)

    FetchContent_MakeAvailable(glslang spirv_cross)
endif()

# Dear ImGui: the in-game dev tools
if(PH_DEV_TOOLS)
    ph_fetch(imgui https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
             fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b)
    FetchContent_MakeAvailable(imgui)
    add_library(ph_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
    target_include_directories(ph_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(ph_imgui PUBLIC SDL3::SDL3)
    set_target_properties(ph_imgui PROPERTIES FOLDER third_party)
endif()

# Tracy: timeline profiler. On demand: zones cost almost nothing until the Tracy app connects.
if(PH_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    ph_fetch(tracy https://github.com/wolfpld/tracy/archive/refs/tags/v0.12.2.tar.gz
             09617765ba5ff1aa6da128d9ba3c608166c5ef05ac28e2bb77f791269d444952)
    FetchContent_MakeAvailable(tracy)
    set_target_properties(TracyClient PROPERTIES FOLDER third_party)
    target_compile_options(TracyClient PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>) # its own warnings aren't ours to fix
endif()

# Box2D v3: optional rigid-body physics (ph/physics/rigid_bodies.hpp).
if(PH_BOX2D)
    set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
    set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
    ph_fetch(box2d https://github.com/erincatto/box2d/archive/refs/tags/v3.1.1.tar.gz
             fb6ef914b50f4312d7d921a600eabc12318bb3c55a0b8c0b90608fa4488ef2e4)
    FetchContent_MakeAvailable(box2d)
    set_target_properties(box2d PROPERTIES FOLDER third_party)
    target_compile_options(box2d PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
endif()

# Ogg Opus: optional .opus sounds and music (ph/audio). libogg and libopus build with their own CMake; opusfile's
# release has none, so its four source files are compiled here (no HTTP streaming).
if(PH_OPUS)
    set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    ph_fetch(ogg https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz
             0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664)
    ph_fetch(opus https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
             65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
    ph_fetch(opusfile https://downloads.xiph.org/releases/opus/opusfile-0.12.tar.gz
             118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b)
    set(saved_log_level "${CMAKE_MESSAGE_LOG_LEVEL}")
    set(CMAKE_MESSAGE_LOG_LEVEL NOTICE) # libopus prints a long feature list on every configure
    FetchContent_MakeAvailable(ogg opus)
    set(CMAKE_MESSAGE_LOG_LEVEL "${saved_log_level}")
    FetchContent_GetProperties(opusfile)
    if(NOT opusfile_POPULATED)
        FetchContent_Populate(opusfile)
    endif()
    add_library(opusfile STATIC
        ${opusfile_SOURCE_DIR}/src/info.c
        ${opusfile_SOURCE_DIR}/src/internal.c
        ${opusfile_SOURCE_DIR}/src/opusfile.c
        ${opusfile_SOURCE_DIR}/src/stream.c)
    target_include_directories(opusfile SYSTEM PUBLIC ${opusfile_SOURCE_DIR}/include)
    target_link_libraries(opusfile PUBLIC ogg opus)
    foreach(lib ogg opus opusfile)
        set_target_properties(${lib} PROPERTIES FOLDER third_party)
        target_compile_options(${lib} PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    endforeach()
endif()

# doctest: unit tests
if(PH_BUILD_TESTS)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    ph_fetch(doctest https://github.com/doctest/doctest/archive/refs/tags/v2.5.2.tar.gz
             9189960c2bbbc4f3382ce0773b2bb5f13e3afd8fed47f55f193e11e85a4f9854)
    FetchContent_MakeAvailable(doctest)
endif()
