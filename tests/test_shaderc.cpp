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
