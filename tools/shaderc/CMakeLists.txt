# ph_shaderc: the GLSL -> SPIR-V/MSL compiler as a library (linked into dev builds for live shader reload)
# ph-shaderc: the same as a command-line tool (used by the build to precompile shaders)

if(NOT PH_NEED_SHADER_COMPILER)
    return()
endif()

add_library(ph_shaderc STATIC src/shaderc.cpp)
add_library(ph::shaderc ALIAS ph_shaderc)
target_include_directories(ph_shaderc PUBLIC include)
target_link_libraries(ph_shaderc
    PUBLIC ph_core
    PRIVATE glslang glslang-default-resource-limits spirv-cross-msl spirv-cross-glsl spirv-cross-core)
ph_configure_target(ph_shaderc)
set_target_properties(ph_shaderc PROPERTIES FOLDER tools)

add_executable(ph-shaderc src/main.cpp)
target_link_libraries(ph-shaderc PRIVATE ph_shaderc)
ph_configure_target(ph-shaderc)
set_target_properties(ph-shaderc PROPERTIES FOLDER tools)
