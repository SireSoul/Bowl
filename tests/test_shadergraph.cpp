#include "bowl/shadergraph/shadergraph.hpp"

#if BOWL_TEST_SHADERC
#include "bowl/shaderc/shaderc.hpp"
#endif

#include <doctest/doctest.h>

using namespace bowl;
namespace sg = bowl::shadergraph;

namespace {
#if BOWL_TEST_SHADERC
shaderc::CompileResult compile(const std::string& glsl) {
    shaderc::CompileOptions options;
    options.name = "graph.frag.glsl";
    options.stage = ShaderStage::Fragment;
    return shaderc::compile_glsl(glsl, options);
}
#endif
} // namespace

TEST_CASE("shader graph: joining pins, refusing loops, and the output staying put") {
    sg::Graph g = sg::starter();
    u32 out = g.output_node();
    REQUIRE(out);
    u32 a = g.add("add", {});
    u32 b = g.add("multiply", {});
    CHECK(g.add("no such node", {}) == 0);
    CHECK(g.connect(a, 0, b, 0));
    CHECK_FALSE(g.connect(b, 0, a, 0)); // a loop
    CHECK_FALSE(g.connect(a, 0, a, 1));
    CHECK_FALSE(g.connect(a, 5, b, 1)); // no such pin
    CHECK(g.connect(b, 0, out, 1));
    CHECK(g.link_into(out, 1)->from == b);
    CHECK(g.connect(a, 0, out, 1)); // replaces what was there
    CHECK(g.link_into(out, 1)->from == a);
    g.remove(out);
    CHECK(g.output_node() == out);
    g.remove(a);
    CHECK_FALSE(g.link_into(b, 0));
    CHECK(g.number(*g.node(b), "in:B")[0] == doctest::Approx(1)); // the type's default
}

TEST_CASE("shader graph: saves and loads, links by pin name") {
    sg::Graph g = sg::starter();
    u32 n = g.add("noise", {10, 20});
    g.node(n)->numbers["in:Scale"] = {3, 3, 3, 3};
    g.node(n)->texts["kind"] = "cloudy";
    g.connect(n, 0, g.output_node(), 1);
    json::Value doc = sg::to_json(g);
    auto back = sg::from_json(json::parse(json::stringify(doc)).value);
    REQUIRE(back);
    CHECK(back->nodes.size() == g.nodes.size());
    CHECK(back->links.size() == g.links.size());
    CHECK(back->text(*back->node(n), "kind") == "cloudy");
    CHECK(back->number(*back->node(n), "in:Scale")[0] == doctest::Approx(3));
    CHECK(back->next_id == g.next_id);
    std::string error;
    CHECK_FALSE(sg::from_json(json::parse(R"({"nodes": [{"id": 1, "type": "sparkle"}]})").value, &error));
    CHECK(error.find("sparkle") != std::string::npos);
}

TEST_CASE("shader graph: parameters become the material's, with their defaults; bad names are problems") {
    sg::Graph g = sg::starter();
    u32 p = g.add("param_color", {});
    g.node(p)->texts["name"] = "glow";
    g.node(p)->numbers["value"] = {1, 0.5f, 0.25f, 1};
    u32 q = g.add("param_number", {});
    g.node(q)->texts["name"] = "2fast";
    u32 m = g.add("multiply", {});
    g.connect(p, 0, m, 0);
    g.connect(q, 0, m, 1);
    g.connect(m, 0, g.output_node(), 2);
    sg::Generated gen = sg::generate(g, "shaders/glow.shadergraph");
    CHECK(gen.glsl.find("vec4 glow; // color = 1.0, 0.5, 0.25, 1.0") != std::string::npos);
    CHECK(gen.glsl.find("shaders/glow.shadergraph") != std::string::npos);
    REQUIRE(gen.problems.size() == 1);
    CHECK(gen.problems[0].find("2fast") != std::string::npos);
#if BOWL_TEST_SHADERC
    auto r = compile(gen.glsl);
    INFO(gen.glsl);
    INFO(r.log);
    REQUIRE(r.ok);
    const auto* glow = r.bundle.uniform("glow");
    REQUIRE(glow);
    CHECK(glow->color);
    CHECK(glow->value[1] == doctest::Approx(0.5f));
#endif
}

#if BOWL_TEST_SHADERC
TEST_CASE("shader graph: every kind of node makes code that compiles, with all its outputs used") {
    for (const sg::NodeType& t : sg::node_types()) {
        if (t.id == "output") continue;
        // The node with every output summed into the output's color.
        sg::Graph g = sg::starter();
        u32 n = g.add(t.id, {});
        if (t.id == "texture") g.node(n)->texts["name"] = "u_noise"; // an extra texture
        u32 sum = 0;
        for (u32 o = 0; o < t.outputs.size(); ++o) {
            u32 add = g.add("add", {});
            REQUIRE(g.connect(n, o, add, 0));
            if (sum) REQUIRE(g.connect(sum, 0, add, 1));
            sum = add;
        }
        if (sum) REQUIRE(g.connect(sum, 0, g.output_node(), 0));
        sg::Generated gen = sg::generate(g, "test");
        CHECK(gen.problems.empty());
        auto r = compile(gen.glsl);
        INFO(t.id);
        INFO(gen.glsl);
        INFO(r.log);
        CHECK(r.ok);
        if (r.ok) CHECK(r.bundle.samplers >= 1);
    }
}

TEST_CASE("shader graph: the starter compiles, keeps the sprite texture at binding 0, and extra textures follow") {
    sg::Graph g = sg::starter();
    auto r = compile(sg::generate(g, "start").glsl);
    INFO(r.log);
    REQUIRE(r.ok);
    CHECK(r.bundle.samplers == 1);

    // A graph that reads only an extra texture still has the sprite's at binding 0.
    sg::Graph h;
    u32 out = h.add("output", {});
    u32 tex = h.add("texture", {});
    h.node(tex)->texts["name"] = "u_clouds";
    h.connect(tex, 0, out, 0);
    auto s = compile(sg::generate(h, "clouds").glsl);
    INFO(s.log);
    REQUIRE(s.ok);
    REQUIRE(s.bundle.textures.size() == 2);
    CHECK(s.bundle.textures[0].name == "u_texture");
    CHECK(s.bundle.textures[1].name == "u_clouds");
}
#endif
