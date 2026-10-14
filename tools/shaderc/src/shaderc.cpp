#include "ph/shaderc/shaderc.hpp"

#include "ph/core/file.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>

namespace ph::shaderc {

namespace fs = std::filesystem;

namespace {

void init_glslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

// Resolves #include "file" relative to the including file, then the root shader's folder.
class Includer final : public glslang::TShader::Includer {
public:
    explicit Includer(fs::path root_dir) : root_dir_(std::move(root_dir)) {}

    IncludeResult* includeLocal(const char* header, const char* includer, size_t) override {
        fs::path base = includer && *includer ? fs::path(includer).parent_path() : root_dir_;
        for (const fs::path& dir : {base, root_dir_}) {
            fs::path candidate = dir / header;
            if (auto text = read_file_text(candidate)) {
                auto* owned = new std::string(std::move(*text));
                std::string name = candidate.lexically_normal().string();
                includes.push_back(candidate.lexically_normal());
                return new IncludeResult(name, owned->data(), owned->size(), owned);
            }
        }
        return nullptr;
    }
    IncludeResult* includeSystem(const char* header, const char* includer, size_t depth) override {
        return includeLocal(header, includer, depth);
    }
    void releaseInclude(IncludeResult* result) override {
        if (!result) return;
        delete static_cast<std::string*>(result->userData);
        delete result;
    }

    std::vector<fs::path> includes;

private:
    fs::path root_dir_;
};

EShLanguage to_glslang(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex: return EShLangVertex;
    case ShaderStage::Fragment: return EShLangFragment;
    case ShaderStage::Compute: return EShLangCompute;
    }
    return EShLangVertex;
}

struct Resource {
    spirv_cross::ID id;
    std::string name;
    u32 set, binding;
};

std::vector<Resource> collect(const spirv_cross::Compiler& c, const spirv_cross::SmallVector<spirv_cross::Resource>& list) {
    std::vector<Resource> out;
    for (const auto& r : list)
        out.push_back({r.id, r.name, c.get_decoration(r.id, spv::DecorationDescriptorSet),
                       c.get_decoration(r.id, spv::DecorationBinding)});
    std::sort(out.begin(), out.end(), [](const Resource& a, const Resource& b) { return a.binding < b.binding; });
    return out;
}

} // namespace

std::optional<ShaderStage> stage_from_filename(std::string_view filename) {
    if (filename.ends_with(".vert.glsl") || filename.ends_with(".vert")) return ShaderStage::Vertex;
    if (filename.ends_with(".frag.glsl") || filename.ends_with(".frag")) return ShaderStage::Fragment;
    if (filename.ends_with(".comp.glsl") || filename.ends_with(".comp")) return ShaderStage::Compute;
    return std::nullopt;
}

CompileResult compile_glsl(std::string_view source, const CompileOptions& options) {
    CompileResult result;
    if (options.stage == ShaderStage::Compute) {
        result.log = "compute shaders are not supported by ph-shaderc yet";
        return result;
    }
    init_glslang();

    // --- GLSL -> SPIR-V -------------------------------------------------------------------------------------------
    EShLanguage lang = to_glslang(options.stage);
    glslang::TShader shader(lang);
    const char* src = source.data();
    int len = static_cast<int>(source.size());
    std::string file_name = options.source_path.empty() ? options.name : options.source_path.string();
    const char* name = file_name.c_str();
    shader.setStringsWithLengthsAndNames(&src, &len, &name, 1);
    shader.setPreamble("#extension GL_GOOGLE_include_directive : require\n");
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");

    Includer includer(options.source_path.empty() ? fs::current_path() : options.source_path.parent_path());
    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    bool parsed = shader.parse(GetDefaultResources(), 450, false, messages, includer);
    result.includes = includer.includes;
    result.log = shader.getInfoLog();
    if (!parsed) return result;

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log += program.getInfoLog();
        return result;
    }

    std::vector<u32> spirv;
    glslang::SpvOptions spv_options;
    spv_options.generateDebugInfo = options.debug_info;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &spv_options);

    // --- reflection: SDL_GPU needs resource counts, and wants them in specific descriptor sets --------------------
    spirv_cross::CompilerMSL msl(spirv);
    auto resources = msl.get_shader_resources();
    auto sampled = collect(msl, resources.sampled_images);
    auto storage_images = collect(msl, resources.storage_images);
    auto storage_buffers = collect(msl, resources.storage_buffers);
    auto uniforms = collect(msl, resources.uniform_buffers);

    u32 resource_set = options.stage == ShaderStage::Vertex ? 0u : 2u;
    u32 uniform_set = resource_set + 1;
    const char* stage_name = options.stage == ShaderStage::Vertex ? "vertex" : "fragment";
    auto check = [&](const std::vector<Resource>& list, u32 set, const char* what) {
        for (const Resource& r : list)
            if (r.set != set) {
                result.log += std::format("{}: {} '{}' must use set = {} in a {} shader (it uses set = {})\n",
                                          options.name, what, r.name, set, stage_name, r.set);
                return false;
            }
        return true;
    };
    if (!check(sampled, resource_set, "sampled texture") || !check(storage_images, resource_set, "storage texture") ||
        !check(storage_buffers, resource_set, "storage buffer") || !check(uniforms, uniform_set, "uniform buffer"))
        return result;

    ShaderBundle& bundle = result.bundle;
    bundle.stage = options.stage;
    bundle.samplers = static_cast<u32>(sampled.size());
    bundle.storage_textures = static_cast<u32>(storage_images.size());
    bundle.storage_buffers = static_cast<u32>(storage_buffers.size());
    bundle.uniform_buffers = static_cast<u32>(uniforms.size());

    ShaderBundle::Blob spirv_blob{ShaderBlobFormat::SpirV, "main", {}};
    spirv_blob.code.resize(spirv.size() * sizeof(u32));
    std::memcpy(spirv_blob.code.data(), spirv.data(), spirv_blob.code.size());
    bundle.blobs.push_back(std::move(spirv_blob));

    // --- SPIR-V -> MSL, with Metal slots laid out the way SDL_GPU binds them --------------------------------------
    auto model = options.stage == ShaderStage::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
    auto bind = [&](const Resource& r, u32 buffer, u32 texture, u32 sampler) {
        spirv_cross::MSLResourceBinding b;
        b.stage = model;
        b.desc_set = r.set;
        b.binding = r.binding;
        b.msl_buffer = buffer;
        b.msl_texture = texture;
        b.msl_sampler = sampler;
        msl.add_msl_resource_binding(b);
    };
    for (u32 i = 0; i < sampled.size(); ++i) bind(sampled[i], 0, i, i);
    for (u32 i = 0; i < storage_images.size(); ++i) bind(storage_images[i], 0, bundle.samplers + i, 0);
    for (u32 i = 0; i < uniforms.size(); ++i) bind(uniforms[i], i, 0, 0);
    for (u32 i = 0; i < storage_buffers.size(); ++i) bind(storage_buffers[i], bundle.uniform_buffers + i, 0, 0);

    auto msl_options = msl.get_msl_options();
    msl_options.set_msl_version(2, 1);
    msl.set_msl_options(msl_options);
    try {
        std::string msl_source = msl.compile();
        ShaderBundle::Blob msl_blob{ShaderBlobFormat::Msl, msl.get_cleansed_entry_point_name("main", model), {}};
        msl_blob.code.assign(msl_source.begin(), msl_source.end());
        bundle.blobs.push_back(std::move(msl_blob));
    } catch (const std::exception& e) {
        result.log += std::format("{}: SPIR-V -> MSL failed: {}\n", options.name, e.what());
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ph::shaderc
