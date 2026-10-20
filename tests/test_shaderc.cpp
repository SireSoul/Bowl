#if BOWL_TEST_SHADERC

#include "bowl/core/file.hpp"
#include "bowl/shaderc/shaderc.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

using namespace bowl;
namespace fs = std::filesystem;

namespace {
shaderc::CompileResult compile_engine_shader(const char* file) {
    fs::path path = fs::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "shaders" / file;
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

TEST_CASE("shaderc: game shaders include the engine's through an include root") {
    const char* source = R"(#version 450
#include "engine/shaders/common.glsl"
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() { out_color = premultiply(v_color); })";
    shaderc::CompileOptions options;
    options.name = "game.frag";
    options.stage = ShaderStage::Fragment;
    options.source_path = fs::temp_directory_path() / "game.frag.glsl"; // not next to the engine's shaders
    auto without = shaderc::compile_glsl(source, options);
    CHECK_FALSE(without.ok);
    options.include_roots.emplace_back("engine/shaders/", fs::path(BOWL_ENGINE_ASSET_SOURCE_DIR) / "shaders");
    auto with = shaderc::compile_glsl(source, options);
    INFO(with.log);
    CHECK(with.ok);
    REQUIRE(with.includes.size() == 1);
    CHECK(with.includes[0].filename() == "common.glsl");
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

TEST_CASE("shaderc: a material's parameters, their defaults and its textures come with the bundle") {
    const char* source = R"(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_noise; // Clouds to scroll
layout(std140, set = 3, binding = 0) uniform Params {
    vec4 glow;       // color = 1, 0.5, 0.25   The glow's color
    float speed;     // = 2 [0, 10] How fast it moves
    float bowl_time;
    vec2 scale;      // = 3
    int steps;       // [1, 8]
};
void main() {
    vec4 n = texture(u_noise, v_uv * scale + vec2(bowl_time * speed));
    out_color = texture(u_texture, v_uv) * glow * n.r * float(steps);
})";
    shaderc::CompileOptions options;
    options.name = "glow.frag";
    options.stage = ShaderStage::Fragment;
    auto r = shaderc::compile_glsl(source, options);
    INFO(r.log);
    REQUIRE(r.ok);
    const ShaderBundle& b = r.bundle;
    CHECK(b.block_size == 48);
    REQUIRE(b.uniforms.size() == 5);
    const auto* glow = b.uniform("glow");
    REQUIRE(glow);
    CHECK(glow->type == ShaderBundle::ValueType::Vec4);
    CHECK(glow->color);
    CHECK(glow->has_default);
    CHECK(glow->value[1] == doctest::Approx(0.5f));
    CHECK(glow->value[3] == doctest::Approx(1)); // a color's missing alpha
    CHECK(glow->help == "The glow's color");
    const auto* speed = b.uniform("speed");
    REQUIRE(speed);
    CHECK(speed->offset == 16);
    CHECK(speed->value[0] == doctest::Approx(2));
    CHECK(speed->min == doctest::Approx(0));
    CHECK(speed->max == doctest::Approx(10));
    CHECK(speed->help == "How fast it moves");
    CHECK_FALSE(b.uniform("bowl_time")->has_default);
    CHECK(b.uniform("scale")->value[1] == doctest::Approx(3)); // one number fills every component
    CHECK(b.uniform("scale")->offset == 24);
    CHECK(b.uniform("steps")->type == ShaderBundle::ValueType::Int);
    CHECK(b.uniform("steps")->max == doctest::Approx(8));
    REQUIRE(b.textures.size() == 2);
    CHECK(b.textures[1].name == "u_noise");
    CHECK(b.textures[1].help == "Clouds to scroll");

    // It all survives the file, and writes into a block.
    auto again = ShaderBundle::parse(b.serialize());
    REQUIRE(again);
    CHECK(again->block_size == 48);
    REQUIRE(again->uniforms.size() == 5);
    CHECK(again->uniforms[1].help == "How fast it moves");
    CHECK(again->uniforms[0].color);
    CHECK(again->textures[1].name == "u_noise");
    std::vector<u8> block(again->block_size, 0);
    const f32 three[] = {3.6f};
    ShaderBundle::write(block, *again->uniform("steps"), three);
    i32 steps = 0;
    std::memcpy(&steps, block.data() + again->uniform("steps")->offset, 4);
    CHECK(steps == 4);
}

TEST_CASE("shaderc: stage from file name") {
    CHECK(shaderc::stage_from_filename("a.vert.glsl") == ShaderStage::Vertex);
    CHECK(shaderc::stage_from_filename("a.frag.glsl") == ShaderStage::Fragment);
    CHECK_FALSE(shaderc::stage_from_filename("common.glsl"));
}

#endif
