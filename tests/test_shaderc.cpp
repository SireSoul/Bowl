#if PH_TEST_SHADERC

#include "ph/core/file.hpp"
#include "ph/shaderc/shaderc.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace ph;
namespace fs = std::filesystem;

namespace {
shaderc::CompileResult compile_engine_shader(const char* file) {
    fs::path path = fs::path(PH_ENGINE_ASSET_SOURCE_DIR) / "shaders" / file;
    auto source = read_file_text(path);
    REQUIRE(source);
    shaderc::CompileOptions options;
    options.name = file;
    options.stage = *shaderc::stage_from_filename(file);
    options.source_path = path;
    return shaderc::compile_glsl(*source, options);
}
} // namespace

TEST_CASE("shaderc: the sprite vertex shader compiles for every backend with SDL's resource layout") {
    auto r = compile_engine_shader("sprite.vert.glsl");
    INFO(r.log);
    REQUIRE(r.ok);
    CHECK(r.bundle.stage == ShaderStage::Vertex);
    CHECK(r.bundle.storage_buffers == 1);
    CHECK(r.bundle.uniform_buffers == 1);
    CHECK(r.bundle.samplers == 0);
    REQUIRE(r.bundle.find(ShaderBlobFormat::SpirV));
    const auto* msl = r.bundle.find(ShaderBlobFormat::Msl);
    REQUIRE(msl);
    std::string src(msl->code.begin(), msl->code.end());
    // Metal: uniforms first, then storage buffers.
    CHECK(src.find("[[buffer(0)]]") != std::string::npos);
    CHECK(src.find("[[buffer(1)]]") != std::string::npos);
    CHECK(msl->entry_point == "main0");
    REQUIRE(r.includes.size() == 1);
    CHECK(r.includes[0].filename() == "common.glsl");
}

TEST_CASE("shaderc: the sprite fragment shader") {
    auto r = compile_engine_shader("sprite.frag.glsl");
    INFO(r.log);
    REQUIRE(r.ok);
    CHECK(r.bundle.samplers == 1);
    CHECK(r.bundle.uniform_buffers == 0);
}

TEST_CASE("shaderc: HLSL for D3D12 uses SDL's register spaces") {
    auto vs = compile_engine_shader("sprite.vert.glsl");
    REQUIRE(vs.ok);
    const auto* v = vs.bundle.find(ShaderBlobFormat::Hlsl);
    REQUIRE(v);
    std::string vsrc(v->code.begin(), v->code.end());
    CHECK(vsrc.find("register(t0, space0)") != std::string::npos); // sprite storage buffer
    CHECK(vsrc.find("register(b0, space1)") != std::string::npos); // camera uniforms

    auto fs = compile_engine_shader("post/final.frag.glsl");
    REQUIRE(fs.ok);
    CHECK(fs.bundle.samplers == 3);
    const auto* f = fs.bundle.find(ShaderBlobFormat::Hlsl);
    REQUIRE(f);
    std::string fsrc(f->code.begin(), f->code.end());
    for (const char* reg : {"register(t0, space2)", "register(t2, space2)", "register(s2, space2)", "register(b0, space3)"})
        CHECK(fsrc.find(reg) != std::string::npos);
}

TEST_CASE("shaderc: compute shaders use SDL's compute layout") {
    auto r = compile_engine_shader("particles.comp.glsl");
    INFO(r.log);
    REQUIRE(r.ok);
    CHECK(r.bundle.stage == ShaderStage::Compute);
    CHECK(r.bundle.readwrite_storage_buffers == 2);
    CHECK(r.bundle.storage_buffers == 0);
    CHECK(r.bundle.uniform_buffers == 1);
    CHECK(r.bundle.threads[0] == 64);
    CHECK(r.bundle.threads[1] == 1);
    for (auto f : {ShaderBlobFormat::SpirV, ShaderBlobFormat::Msl, ShaderBlobFormat::Hlsl}) CHECK(r.bundle.find(f));
    const auto* h = r.bundle.find(ShaderBlobFormat::Hlsl);
    REQUIRE(h);
    std::string hlsl(h->code.begin(), h->code.end());
    for (const char* reg : {"register(u0, space1)", "register(u1, space1)", "register(b0, space2)"})
        CHECK(hlsl.find(reg) != std::string::npos);

    // Read-only and read-write storage, and a wrong set.
    const char* both = R"(#version 450
layout(local_size_x = 8, local_size_y = 4) in;
layout(std430, set = 0, binding = 0) readonly buffer In { float a[]; };
layout(std430, set = 1, binding = 0) buffer Out { float b[]; };
void main() { b[gl_GlobalInvocationID.x] = a[gl_GlobalInvocationID.x] * 2.0; })";
    shaderc::CompileOptions options;
    options.name = "double.comp";
    options.stage = ShaderStage::Compute;
    auto d = shaderc::compile_glsl(both, options);
    INFO(d.log);
    REQUIRE(d.ok);
    CHECK(d.bundle.storage_buffers == 1);
    CHECK(d.bundle.readwrite_storage_buffers == 1);
    CHECK(d.bundle.threads[1] == 4);
    auto again = ShaderBundle::parse(d.bundle.serialize());
    REQUIRE(again);
    CHECK(again->readwrite_storage_buffers == 1);
    CHECK(again->threads[0] == 8);

    std::string wrong = both;
    wrong.replace(wrong.find("set = 1"), 7, "set = 0");
    auto w = shaderc::compile_glsl(wrong, options);
    CHECK_FALSE(w.ok);
    CHECK(w.log.find("set = 1") != std::string::npos);
}

TEST_CASE("shaderc: errors are reported, not thrown") {
    shaderc::CompileOptions options;
    options.name = "broken.frag.glsl";
    options.stage = ShaderStage::Fragment;
    auto r = shaderc::compile_glsl("#version 450\nvoid main() { undefined_thing(); }\n", options);
    CHECK_FALSE(r.ok);
    CHECK(r.log.find("undefined_thing") != std::string::npos);
}

TEST_CASE("shaderc: wrong descriptor set gets a helpful message") {
    shaderc::CompileOptions options;
    options.name = "wrong.frag.glsl";
    options.stage = ShaderStage::Fragment;
    auto r = shaderc::compile_glsl(
        "#version 450\nlayout(set = 0, binding = 0) uniform sampler2D t;\nlayout(location = 0) out vec4 c;\n"
        "void main() { c = texture(t, vec2(0)); }\n",
        options);
    CHECK_FALSE(r.ok);
    CHECK(r.log.find("set = 2") != std::string::npos);
}

TEST_CASE("shaderc: stage from file name") {
    CHECK(shaderc::stage_from_filename("a.vert.glsl") == ShaderStage::Vertex);
    CHECK(shaderc::stage_from_filename("a.frag.glsl") == ShaderStage::Fragment);
    CHECK_FALSE(shaderc::stage_from_filename("common.glsl"));
}

#endif
