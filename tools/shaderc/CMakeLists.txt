# bowl_shaderc: the GLSL -> SPIR-V/MSL compiler as a library (linked into dev builds for live shader reload)
# bowl-shaderc: the same as a command-line tool (used by the build to precompile shaders)

if(NOT BOWL_NEED_SHADER_COMPILER)
    return()
endif()

add_library(bowl_shaderc STATIC src/shaderc.cpp)
add_library(bowl::shaderc ALIAS bowl_shaderc)
target_include_directories(bowl_shaderc PUBLIC include)
target_link_libraries(bowl_shaderc
    PUBLIC bowl_core
    PRIVATE glslang glslang-default-resource-limits spirv-cross-msl spirv-cross-hlsl spirv-cross-glsl spirv-cross-core)
bowl_configure_target(bowl_shaderc)
set_target_properties(bowl_shaderc PROPERTIES FOLDER tools)

add_executable(bowl-shaderc src/main.cpp)
target_link_libraries(bowl-shaderc PRIVATE bowl_shaderc)
bowl_configure_target(bowl-shaderc)
set_target_properties(bowl-shaderc PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
