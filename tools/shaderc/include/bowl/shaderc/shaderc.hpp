#pragma once

#include "bowl/core/shader_bundle.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bowl::shaderc {

struct CompileOptions {
    std::string name = "shader";         // shown in error messages
    ShaderStage stage = ShaderStage::Vertex;
    std::filesystem::path source_path;   // for resolving #include "..." (relative to this file's folder)
    bool debug_info = false;
    // More places to find includes: an include starting with the prefix is looked for in the folder, e.g.
    // {"engine/shaders/", <engine shader folder>} lets game shaders write #include "engine/shaders/common.glsl".
    std::vector<std::pair<std::string, std::filesystem::path>> include_roots;
};

struct CompileResult {
    bool ok = false;
    std::string log;                              // errors and warnings
    ShaderBundle bundle;
    std::vector<std::filesystem::path> includes;  // every file #included, for hot reload and depfiles
};

// Compiles Vulkan-style GLSL (#version 450) into a bundle with SPIR-V, Metal Shading Language and HLSL.
// Resources must follow SDL_GPU's binding model:
//   vertex:   set 0 = sampled textures, storage textures, storage buffers   set 1 = uniform buffers
//   fragment: set 2 = sampled textures, storage textures, storage buffers   set 3 = uniform buffers
CompileResult compile_glsl(std::string_view source, const CompileOptions& options);

// "sprite.vert.glsl" -> Vertex, "sprite.frag.glsl" -> Fragment, "x.comp.glsl" -> Compute.
std::optional<ShaderStage> stage_from_filename(std::string_view filename);

} // namespace bowl::shaderc
