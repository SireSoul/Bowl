#include "bowl/shaderc/shaderc.hpp"

#include "bowl/core/file.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>

namespace bowl::shaderc {

namespace fs = std::filesystem;

namespace {

void init_glslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

// Resolves #include "file" relative to the including file, then the root shader's folder, then the include roots.
class Includer final : public glslang::TShader::Includer {
public:
    Includer(fs::path root_dir, const std::vector<std::pair<std::string, fs::path>>& roots)
        : root_dir_(std::move(root_dir)), roots_(roots) {}

    IncludeResult* includeLocal(const char* header, const char* includer, size_t) override {
        fs::path base = includer && *includer ? fs::path(includer).parent_path() : root_dir_;
        std::vector<fs::path> candidates{base / header, root_dir_ / header};
        std::string_view name_in_source = header;
        for (const auto& [prefix, dir] : roots_)
            if (name_in_source.starts_with(prefix)) candidates.push_back(dir / name_in_source.substr(prefix.size()));
        for (const fs::path& candidate : candidates) {
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
    const std::vector<std::pair<std::string, fs::path>>& roots_;
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

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The comment after the declaration of `name` in the shader's own source: a line whose code is "<type> name;" (or
// "uniform sampler2D name;", an array, a layout qualifier before it). Empty when there's none.
std::string_view comment_of(std::string_view source, std::string_view name) {
    usize at = 0;
    while (at < source.size()) {
        usize end = source.find('\n', at);
        if (end == std::string_view::npos) end = source.size();
        std::string_view line = source.substr(at, end - at);
        at = end + 1;
        usize slash = line.find("//");
        if (slash == std::string_view::npos) continue;
        std::string_view code = trim(line.substr(0, slash));
        if (code.empty() || code.back() != ';') continue;
        code = trim(code.substr(0, code.size() - 1));
        if (code.ends_with(']')) code = trim(code.substr(0, code.rfind('[')));
        if (code.starts_with("layout")) code = trim(code.substr(std::min(code.size(), code.find(')') + 1)));
        if (!code.ends_with(name)) continue;
        usize before = code.size() - name.size();
        if (before == 0 || is_ident(code[before - 1]) || code.substr(0, before).find_first_of("=(+*/") != std::string_view::npos)
            continue;
        return trim(line.substr(slash + 2));
    }
    return {};
}

// "color = 1, 0.6, 0.2 [0, 4] What it's for": the parts in any order, then the help.
void annotate(std::string_view text, u32 components, ShaderBundle::Uniform& u) {
    auto number = [&](std::string_view& t, f32& out) {
        t = trim(t);
        std::string tmp(t.substr(0, std::min<usize>(t.size(), 32)));
        char* end = nullptr;
        out = std::strtof(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        t.remove_prefix(static_cast<usize>(end - tmp.c_str()));
        t = trim(t);
        if (!t.empty() && t.front() == ',') t = trim(t.substr(1));
        return true;
    };
    for (;;) {
        text = trim(text);
        if (text.size() >= 5 && (text.substr(0, 5) == "color" || text.substr(0, 5) == "Color") &&
            (text.size() == 5 || !is_ident(text[5]))) {
            u.color = true;
            text.remove_prefix(5);
        } else if (!text.empty() && text.front() == '=') {
            text.remove_prefix(1);
            u32 n = 0;
            f32 v = 0;
            while (n < 4 && number(text, v)) u.value[n++] = v;
            if (n == 0) break;
            u.has_default = true;
            if (n == 1)
                for (u32 i = 1; i < 4; ++i) u.value[i] = u.value[0];
            else if (u.color && n == 3 && components == 4) u.value[3] = 1;
        } else if (!text.empty() && text.front() == '[') {
            std::string_view t = text.substr(1);
            f32 lo = 0, hi = 0;
            if (!number(t, lo) || !number(t, hi) || t.empty() || t.front() != ']') break;
            u.min = lo;
            u.max = hi;
            text = t.substr(1);
        } else {
            break;
        }
    }
    u.help = std::string(trim(text));
}

// The uniform block at binding 0 member by member, and the sampled textures by binding, for materials to fill by
// name (ShaderBundle::uniforms, ::textures).
void reflect(const spirv_cross::Compiler& c, const spirv_cross::ShaderResources& resources, std::string_view source,
             ShaderBundle& bundle) {
    for (const auto& r : resources.uniform_buffers) {
        if (c.get_decoration(r.id, spv::DecorationBinding) != 0) continue;
        const spirv_cross::SPIRType& block = c.get_type(r.base_type_id);
        bundle.block_size = (static_cast<u32>(c.get_declared_struct_size(block)) + 15) & ~15u; // std140 pads to 16
        for (u32 i = 0; i < block.member_types.size(); ++i) {
            const spirv_cross::SPIRType& t = c.get_type(block.member_types[i]);
            ShaderBundle::Uniform u;
            u.name = c.get_member_name(r.base_type_id, i);
            u.offset = c.type_struct_member_offset(block, i);
            u.size = static_cast<u32>(c.get_declared_struct_member_size(block, i));
            u.count = t.array.empty() ? 1 : std::max(1u, t.array[0]);
            using T = ShaderBundle::ValueType;
            u32 n = t.vecsize;
            if (t.columns == 4 && n == 4 && t.basetype == spirv_cross::SPIRType::Float) u.type = T::Mat4;
            else if (t.columns > 1) u.type = T::Other;
            else if (t.basetype == spirv_cross::SPIRType::Float) u.type = static_cast<T>(static_cast<u32>(T::Float) + n - 1);
            else if (t.basetype == spirv_cross::SPIRType::Int) u.type = static_cast<T>(static_cast<u32>(T::Int) + n - 1);
            else if (t.basetype == spirv_cross::SPIRType::UInt && n == 1) u.type = T::UInt;
            else u.type = T::Other;
            annotate(comment_of(source, u.name), n, u);
            bundle.uniforms.push_back(std::move(u));
        }
    }
    std::vector<std::pair<u32, std::string>> sampled;
    for (const auto& r : resources.sampled_images) sampled.emplace_back(c.get_decoration(r.id, spv::DecorationBinding), r.name);
    std::sort(sampled.begin(), sampled.end());
    for (auto& [binding, name] : sampled)
        bundle.textures.push_back({name, std::string(comment_of(source, name))});
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

    Includer includer(options.source_path.empty() ? fs::current_path() : options.source_path.parent_path(),
                      options.include_roots);
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
    reflect(msl, resources, source, bundle);

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

} // namespace bowl::shaderc
