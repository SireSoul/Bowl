#include "ph/shaderc/shaderc.hpp"

#include "ph/core/file.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_hlsl.hpp>
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

// Compute shaders, in SDL_GPU's layout for them:
//   set 0: sampled textures, then read-only storage textures, then read-only storage buffers
//   set 1: read-write storage textures, then read-write storage buffers
//   set 2: uniform buffers
// Storage marked `readonly` in GLSL is read-only; anything else is read-write.
bool finish_compute(const std::vector<u32>& spirv, const CompileOptions& options, CompileResult& result) {
    spirv_cross::CompilerMSL msl(spirv);
    auto resources = msl.get_shader_resources();
    auto sampled = collect(msl, resources.sampled_images);
    auto images = collect(msl, resources.storage_images);
    auto buffers = collect(msl, resources.storage_buffers);
    auto uniforms = collect(msl, resources.uniform_buffers);

    std::vector<Resource> ro_images, rw_images, ro_buffers, rw_buffers;
    for (const Resource& r : images)
        (msl.has_decoration(r.id, spv::DecorationNonWritable) ? ro_images : rw_images).push_back(r);
    for (const Resource& r : buffers)
        (msl.get_buffer_block_flags(r.id).get(spv::DecorationNonWritable) ? ro_buffers : rw_buffers).push_back(r);

    auto check = [&](const std::vector<Resource>& list, u32 set, const char* what) {
        for (const Resource& r : list)
            if (r.set != set) {
                result.log += std::format("{}: {} '{}' must use set = {} in a compute shader (it uses set = {})\n",
                                          options.name, what, r.name, set, r.set);
                return false;
            }
        return true;
    };
    if (!check(sampled, 0, "sampled texture") || !check(ro_images, 0, "readonly storage texture") ||
        !check(ro_buffers, 0, "readonly storage buffer") || !check(rw_images, 1, "read-write storage texture") ||
        !check(rw_buffers, 1, "read-write storage buffer") || !check(uniforms, 2, "uniform buffer"))
        return false;

    ShaderBundle& bundle = result.bundle;
    bundle.stage = ShaderStage::Compute;
    bundle.samplers = static_cast<u32>(sampled.size());
    bundle.storage_textures = static_cast<u32>(ro_images.size());
    bundle.storage_buffers = static_cast<u32>(ro_buffers.size());
    bundle.readwrite_storage_textures = static_cast<u32>(rw_images.size());
    bundle.readwrite_storage_buffers = static_cast<u32>(rw_buffers.size());
    bundle.uniform_buffers = static_cast<u32>(uniforms.size());
    for (u32 i = 0; i < 3; ++i) bundle.threads[i] = std::max(1u, msl.get_execution_mode_argument(spv::ExecutionModeLocalSize, i));

    ShaderBundle::Blob spirv_blob{ShaderBlobFormat::SpirV, "main", {}};
    spirv_blob.code.resize(spirv.size() * sizeof(u32));
    std::memcpy(spirv_blob.code.data(), spirv.data(), spirv_blob.code.size());
    bundle.blobs.push_back(std::move(spirv_blob));

    // MSL: [[texture]] sampled, read-only, read-write textures; [[sampler]] with the sampled textures; [[buffer]]
    // uniforms, read-only, read-write buffers.
    auto model = spv::ExecutionModelGLCompute;
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
    u32 textures = 0, buffer_slot = 0;
    for (u32 i = 0; i < sampled.size(); ++i) bind(sampled[i], 0, textures++, i);
    for (const Resource& r : ro_images) bind(r, 0, textures++, 0);
    for (const Resource& r : rw_images) bind(r, 0, textures++, 0);
    for (const Resource& r : uniforms) bind(r, buffer_slot++, 0, 0);
    for (const Resource& r : ro_buffers) bind(r, buffer_slot++, 0, 0);
    for (const Resource& r : rw_buffers) bind(r, buffer_slot++, 0, 0);
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
        return false;
    }

    // HLSL: t[] (space0) sampled, read-only textures, read-only buffers; s[] with the sampled textures; u[] (space1)
    // read-write textures, then buffers; b[] (space2) uniforms.
    try {
        spirv_cross::CompilerHLSL hlsl(spirv);
        spirv_cross::CompilerHLSL::Options hlsl_options;
        hlsl_options.shader_model = 51;
        hlsl.set_hlsl_options(hlsl_options);
        auto hbind = [&](const Resource& r, char kind, u32 reg) {
            spirv_cross::HLSLResourceBinding b;
            b.stage = model;
            b.desc_set = r.set;
            b.binding = r.binding;
            if (kind == 'b') b.cbv = {r.set, reg};
            else if (kind == 'u') b.uav = {r.set, reg};
            else b.srv = b.sampler = {r.set, reg};
            hlsl.add_hlsl_resource_binding(b);
        };
        u32 t = 0, u = 0;
        for (const Resource& r : sampled) hbind(r, 't', t++);
        for (const Resource& r : ro_images) hbind(r, 't', t++);
        for (const Resource& r : ro_buffers) hbind(r, 't', t++);
        for (const Resource& r : rw_images) hbind(r, 'u', u++);
        for (const Resource& r : rw_buffers) hbind(r, 'u', u++);
        for (u32 i = 0; i < uniforms.size(); ++i) hbind(uniforms[i], 'b', i);
        std::string hlsl_source = hlsl.compile();
        ShaderBundle::Blob hlsl_blob{ShaderBlobFormat::Hlsl, "main", {}};
        hlsl_blob.code.assign(hlsl_source.begin(), hlsl_source.end());
        bundle.blobs.push_back(std::move(hlsl_blob));
    } catch (const std::exception& e) {
        result.log += std::format("{}: SPIR-V -> HLSL failed: {}\n", options.name, e.what());
        return false;
    }
    return true;
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
    init_glslang();

    // GLSL -> SPIR-V
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
    if (options.stage == ShaderStage::Compute) {
        result.ok = finish_compute(spirv, options, result);
        return result;
    }

    // Reflection: SDL_GPU needs resource counts, and wants them in specific descriptor sets
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

    // SPIR-V -> MSL, with Metal slots laid out the way SDL_GPU binds them
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

    // SPIR-V -> HLSL (Shader Model 5.1) for D3D12, in SDL_GPU's register layout
    // t[n]/s[n] in the resource set's space, b[n] in the uniform set's space; storage textures and buffers follow the
    // sampled textures in t[]. Descriptor set numbers already equal SDL's spaces.
    try {
        spirv_cross::CompilerHLSL hlsl(spirv);
        spirv_cross::CompilerHLSL::Options hlsl_options;
        hlsl_options.shader_model = 51;
        hlsl.set_hlsl_options(hlsl_options);
        auto hbind = [&](const Resource& r, bool cbv, u32 reg) {
            spirv_cross::HLSLResourceBinding b;
            b.stage = model;
            b.desc_set = r.set;
            b.binding = r.binding;
            if (cbv) b.cbv = {r.set, reg};
            else b.srv = b.sampler = {r.set, reg};
            hlsl.add_hlsl_resource_binding(b);
        };
        for (u32 i = 0; i < sampled.size(); ++i) hbind(sampled[i], false, i);
        for (u32 i = 0; i < storage_images.size(); ++i) hbind(storage_images[i], false, bundle.samplers + i);
        for (u32 i = 0; i < storage_buffers.size(); ++i)
            hbind(storage_buffers[i], false, bundle.samplers + bundle.storage_textures + i);
        for (u32 i = 0; i < uniforms.size(); ++i) hbind(uniforms[i], true, i);
        std::string hlsl_source = hlsl.compile();
        ShaderBundle::Blob hlsl_blob{ShaderBlobFormat::Hlsl, "main", {}};
        hlsl_blob.code.assign(hlsl_source.begin(), hlsl_source.end());
        bundle.blobs.push_back(std::move(hlsl_blob));
    } catch (const std::exception& e) {
        result.log += std::format("{}: SPIR-V -> HLSL failed: {}\n", options.name, e.what());
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ph::shaderc
