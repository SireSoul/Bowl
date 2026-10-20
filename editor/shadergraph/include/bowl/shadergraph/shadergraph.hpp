#pragma once

#include "bowl/core/json.hpp"
#include "bowl/core/math.hpp"
#include "bowl/core/types.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Bowl Editor's shader graph, without any windows: nodes joined by links, read from and written to .shadergraph
// files, and turned into a sprite material's fragment shader. The Materials workspace draws and drives it;
// tests/test_shadergraph.cpp checks it, compiling what it makes.
namespace bowl::shadergraph {

using Values = std::array<f32, 4>;

// What a pin carries: 1 to 4 numbers (a number, a vector like a UV, a color). In the code a graph makes every value is
// a vec4, the parts a narrower one doesn't use are zero, and a number fills every part, so any output joins any input
// and arithmetic works the way GLSL's own does (a color times a number, a UV plus a vector).
struct Pin {
    std::string name;
    u8 size = 1;        // 1 a number, 2 a vector, 3 a color without alpha, 4 a color
    Values value{};     // an input's value while nothing's joined to it
    bool uv = false;    // an input that takes the sprite's UV while nothing's joined to it
    bool color = false; // shown as a color
};

// Something about a node that isn't joined to anything: a number, a color, a name.
struct Setting {
    enum class Kind : u8 { Number, Color, Text, Choice } kind = Kind::Number;
    std::string key, label;
    u8 size = 1;  // numbers in a Number setting
    Values value{};
    std::string text; // Text and Choice
    std::vector<std::string> options;
    f32 min = 0, max = 0; // a slider's range when max > min
    std::string help;
};

class Writer;

struct NodeType {
    std::string id, title, category, help;
    std::vector<Pin> inputs, outputs;
    std::vector<Setting> settings;
    std::function<void(Writer&)> write; // its code; empty for the output node
};
// Every kind of node, by category.
const std::vector<NodeType>& node_types();
const NodeType* node_type(std::string_view id);

struct Node {
    u32 id = 0;
    std::string type;
    Vec2 position;
    // What's been changed from the type's defaults: settings by key, and unjoined inputs as "in:<pin name>".
    std::map<std::string, Values, std::less<>> numbers;
    std::map<std::string, std::string, std::less<>> texts;
};

struct Link {
    u32 from = 0, output = 0; // a node and one of its outputs
    u32 to = 0, input = 0;    // a node and one of its inputs
    bool operator==(const Link&) const = default;
};

class Graph {
public:
    std::vector<Node> nodes;
    std::vector<Link> links;
    u32 next_id = 1;

    Node* node(u32 id);
    const Node* node(u32 id) const;
    // A new node of a type, at a place on the canvas. 0 if there's no such type.
    u32 add(std::string_view type, Vec2 at);
    // Removes a node and its links. The output node stays.
    void remove(u32 id);
    // Joins an output to an input, replacing what was joined there. False (and nothing changes) when the pins don't
    // exist or it would make a loop.
    bool connect(u32 from, u32 output, u32 to, u32 input);
    void disconnect(u32 to, u32 input);
    const Link* link_into(u32 to, u32 input) const;
    // Whether `to` already feeds `from` (joining from -> to would make a loop).
    bool feeds(u32 from, u32 to) const;
    u32 output_node() const; // every graph has one
    // A setting's value (or an input's own value, "in:<pin>"), from the node or its type's default.
    Values number(const Node& n, std::string_view key) const;
    std::string text(const Node& n, std::string_view key) const;
};

// A new graph: the sprite's texture times its color, out.
Graph starter();
json::Value to_json(const Graph& g);
std::optional<Graph> from_json(const json::Value& v, std::string* error = nullptr);

struct Generated {
    std::string glsl;
    std::vector<std::string> problems; // parameters with names that won't do, too many textures
};
// The fragment shader a graph makes. `source` (the graph's asset path) goes in its first comment.
Generated generate(const Graph& g, std::string_view source);

// What a node type's `write` works with: its inputs as vec4 expressions, its settings, and where its outputs go.
class Writer {
public:
    std::string in(usize i) const;     // input i, a vec4
    std::string part(usize i) const;   // input i cut to its size: "x.x", "x.xy", "x.xyz" or "x"
    u8 size(usize i) const;            // how many of its parts mean something
    u8 widest() const;                 // of every input
    Values number(std::string_view key) const;
    f32 num(std::string_view key) const { return number(key)[0]; }
    std::string text(std::string_view key) const;
    // Output i is a vec4 expression whose first `size` parts mean something.
    void out(usize i, const std::string& expr, u8 size);
    // A line of code before the outputs. Names made with local() are this node's own.
    void line(const std::string& code);
    std::string local(std::string_view part) const;
    void need(std::string_view helper); // a function the code calls: "unpremultiply", "noise", "fbm", "voronoi", "hsv"...
    std::string time();                 // seconds, from bowl_time
    std::string screen();               // bowl_screen: the target's size, and one over it
    std::string sampler(std::string_view name); // a texture by the name its material gives it; u_texture is the sprite's
    std::string param(std::string name, u8 size, Values value, f32 min, f32 max, bool color, std::string help);
    static std::string literal(f32 v);
    static std::string vec4(const Values& v, u8 size);

private:
    friend Generated generate(const Graph& g, std::string_view source);
    struct State;
    State* state_ = nullptr;
    const Graph* graph_ = nullptr;
    const Node* node_ = nullptr;
    std::vector<std::string> in_;
    std::vector<u8> sizes_;
    std::vector<std::string> lines_;
    std::vector<std::pair<std::string, u8>> out_;
};

} // namespace bowl::shadergraph
