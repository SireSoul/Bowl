#include "bowl/shadergraph/shadergraph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace bowl::shadergraph {

namespace {

// --- Helpers the generated code can call ----------------------------------------------------------------------------

struct Helper {
    const char* name;
    std::vector<const char*> needs;
    const char* code;
};

const std::vector<Helper>& helpers() {
    static const std::vector<Helper> list = {
        {"unpremultiply", {}, R"(vec4 unpremultiply(vec4 c) { return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0); })"},
        {"hash", {}, R"(float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
})"},
        {"noise", {"hash"}, R"(float value_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0)), u.x), u.y);
})"},
        {"fbm", {"noise"}, R"(float fbm(vec2 p, int octaves) {
    float v = 0.0, a = 0.5, total = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= octaves) break;
        v += a * value_noise(p);
        total += a;
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return v / max(total, 0.0001);
})"},
        {"voronoi", {"hash"}, R"(vec2 voronoi(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    float best = 8.0, cell = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            float d = length(g + hash22(i + g) - f);
            if (d < best) {
                best = d;
                cell = hash12(i + g);
            }
        }
    return vec2(best, cell);
})"},
        {"hsv", {}, R"(vec3 rgb_to_hsv(vec3 c) {
    vec4 k = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 hsv_to_rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
})"},
        {"rotate", {}, R"(vec2 rotate_uv(vec2 uv, vec2 center, float radians) {
    float c = cos(radians), s = sin(radians);
    vec2 d = uv - center;
    return center + vec2(d.x * c - d.y * s, d.x * s + d.y * c);
})"},
    };
    return list;
}

// --- Node types -----------------------------------------------------------------------------------------------------

Pin in(std::string name, u8 size, Values value = {}, bool color = false) {
    Pin p;
    p.name = std::move(name);
    p.size = size;
    p.value = value;
    p.color = color;
    return p;
}
Pin uv_in(std::string name = "UV") {
    Pin p = in(std::move(name), 2);
    p.uv = true;
    return p;
}
Pin out(std::string name, u8 size, bool color = false) { return in(std::move(name), size, {}, color); }
Values all(f32 v) { return {v, v, v, v}; }

Setting number(std::string key, std::string label, f32 value, f32 min = 0, f32 max = 0, std::string help = {}) {
    Setting s;
    s.kind = Setting::Kind::Number;
    s.key = std::move(key);
    s.label = std::move(label);
    s.value = {value, 0, 0, 0};
    s.min = min;
    s.max = max;
    s.help = std::move(help);
    return s;
}
Setting text(std::string key, std::string label, std::string value, std::string help = {}) {
    Setting s;
    s.kind = Setting::Kind::Text;
    s.key = std::move(key);
    s.label = std::move(label);
    s.text = std::move(value);
    s.help = std::move(help);
    return s;
}
Setting choice(std::string key, std::string label, std::vector<std::string> options, std::string help = {}) {
    Setting s;
    s.kind = Setting::Kind::Choice;
    s.key = std::move(key);
    s.label = std::move(label);
    s.text = options.empty() ? std::string() : options[0];
    s.options = std::move(options);
    s.help = std::move(help);
    return s;
}
Setting color(std::string key, std::string label, Values value) {
    Setting s;
    s.kind = Setting::Kind::Color;
    s.key = std::move(key);
    s.label = std::move(label);
    s.value = value;
    return s;
}

// "x" -> "vec4((x).x)" and so on: a value of `size` parts as a vec4 with the rest zero (a number fills every part).
std::string masked(const std::string& e, u8 size) {
    switch (size) {
    case 1: return "vec4((" + e + ").x)";
    case 2: return "vec4((" + e + ").xy, 0.0, 0.0)";
    case 3: return "vec4((" + e + ").xyz, 0.0)";
    default: return e;
    }
}

std::vector<NodeType> make_types() {
    std::vector<NodeType> types;
    auto add = [&](std::string id, std::string title, std::string category, std::string help, std::vector<Pin> ins,
                   std::vector<Pin> outs, std::function<void(Writer&)> write, std::vector<Setting> settings = {}) {
        types.push_back({std::move(id), std::move(title), std::move(category), std::move(help), std::move(ins), std::move(outs),
                     std::move(settings), std::move(write)});
    };
    // Two inputs, one output as wide as the widest.
    auto binary = [&](std::string id, std::string title, std::string help, Values a, Values b, std::string op,
                      std::string category = "Math") {
        add(std::move(id), std::move(title), std::move(category), std::move(help), {in("A", 1, a), in("B", 1, b)},
            {out("Out", 1)}, [op](Writer& w) {
                std::string e = op;
                for (auto [from, to] : {std::pair<std::string, std::string>{"$a", w.in(0)}, {"$b", w.in(1)}})
                    for (usize at; (at = e.find(from)) != std::string::npos;) e.replace(at, 2, to);
                w.out(0, e, w.widest());
            });
    };
    auto unary = [&](std::string id, std::string title, std::string help, std::string op, Values x = {}) {
        add(std::move(id), std::move(title), "Math", std::move(help), {in("In", 1, x)}, {out("Out", 1)}, [op](Writer& w) {
            std::string e = op;
            for (usize at; (at = e.find("$x")) != std::string::npos;) e.replace(at, 2, w.in(0));
            w.out(0, e, w.size(0));
        });
    };

    // Inputs
    add("uv", "UV", "Input", "Where in the sprite this pixel is: 0 to 1 across and down its picture (or sprite sheet frame)",
        {}, {out("UV", 2)}, [](Writer& w) { w.out(0, "vec4(v_uv, 0.0, 0.0)", 2); });
    add("sprite_color", "Sprite color", "Input", "The sprite's own color: what the game tints it with", {},
        {out("Color", 4, true), out("RGB", 3, true), out("Alpha", 1)}, [](Writer& w) {
            w.need("unpremultiply");
            std::string c = w.local("c");
            w.line("vec4 " + c + " = unpremultiply(v_color);");
            w.out(0, c, 4);
            w.out(1, c, 3);
            w.out(2, "vec4(" + c + ".a)", 1);
        });
    add("texture", "Texture", "Input",
        "A picture's color at a UV. u_texture is the sprite's own picture; any other name is an extra texture, picked in "
        "the material (up to three)",
        {uv_in()}, {out("Color", 4, true), out("RGB", 3, true), out("Alpha", 1)},
        [](Writer& w) {
            w.need("unpremultiply");
            std::string s = w.sampler(w.text("name")), c = w.local("c");
            w.line("vec4 " + c + " = unpremultiply(texture(" + s + ", " + w.in(0) + ".xy));");
            w.out(0, c, 4);
            w.out(1, c, 3);
            w.out(2, "vec4(" + c + ".a)", 1);
        },
        {text("name", "Texture", "u_texture", "u_texture: the sprite's picture. Another name: a texture the material picks")});
    add("time", "Time", "Input", "Seconds since the game started, for anything that moves", {},
        {out("Seconds", 1), out("Sine", 1), out("Cosine", 1)},
        [](Writer& w) {
            std::string t = w.local("t");
            w.line("float " + t + " = " + w.time() + " * " + Writer::literal(w.num("speed")) + ";");
            w.out(0, "vec4(" + t + ")", 1);
            w.out(1, "vec4(sin(" + t + "))", 1);
            w.out(2, "vec4(cos(" + t + "))", 1);
        },
        {number("speed", "Speed", 1, 0, 10, "Seconds go this many times faster")});
    add("screen", "Screen position", "Input", "Where on the screen this pixel is: in pixels, and 0 to 1", {},
        {out("Pixel", 2), out("Screen UV", 2)}, [](Writer& w) {
            w.out(0, "vec4(gl_FragCoord.xy, 0.0, 0.0)", 2);
            w.out(1, "vec4(gl_FragCoord.xy * " + w.screen() + ".zw, 0.0, 0.0)", 2);
        });
    add("texture_size", "Texture size", "Input", "The sprite's picture's size in pixels, and the UV size of one pixel", {},
        {out("Size", 2), out("Pixel UV", 2)}, [](Writer& w) {
            std::string s = w.local("size");
            w.line("vec2 " + s + " = vec2(textureSize(" + w.sampler("u_texture") + ", 0));");
            w.out(0, "vec4(" + s + ", 0.0, 0.0)", 2);
            w.out(1, "vec4(1.0 / " + s + ", 0.0, 0.0)", 2);
        });
    add("number", "Number", "Input", "A number", {}, {out("Value", 1)},
        [](Writer& w) { w.out(0, "vec4(" + Writer::literal(w.num("value")) + ")", 1); }, {number("value", "Value", 1)});
    add("color", "Color", "Input", "A color", {}, {out("Color", 4, true)},
        [](Writer& w) { w.out(0, Writer::vec4(w.number("value"), 4), 4); }, {color("value", "Color", all(1))});
    {
        Setting v = number("value", "Value", 0);
        v.size = 2;
        add("vector", "Vector", "Input", "Two numbers: a direction, a size, a place in UV", {}, {out("Vector", 2)},
            [](Writer& w) { w.out(0, Writer::vec4(w.number("value"), 2), 2); }, {v});
    }
    add("param_number", "Parameter (number)", "Parameters",
        "A number each material sets for itself (a speed, an amount), with a slider in the material", {}, {out("Value", 1)},
        [](Writer& w) {
            std::string p = w.param(w.text("name"), 1, w.number("value"), w.num("min"), w.num("max"), false, w.text("help"));
            w.out(0, "vec4(" + p + ")", 1);
        },
        {text("name", "Name", "amount", "What it's called in the material and the code: letters, digits and _"),
         number("value", "Default", 0.5f), number("min", "Slider from", 0), number("max", "Slider to", 1),
         text("help", "Help", "", "What it's for, shown in the material")});
    add("param_color", "Parameter (color)", "Parameters", "A color each material sets for itself", {},
        {out("Color", 4, true), out("RGB", 3, true)},
        [](Writer& w) {
            std::string p = w.param(w.text("name"), 4, w.number("value"), 0, 0, true, w.text("help"));
            w.out(0, p, 4);
            w.out(1, p, 3);
        },
        {text("name", "Name", "tint", "What it's called in the material and the code: letters, digits and _"),
         color("value", "Default", all(1)), text("help", "Help", "", "What it's for, shown in the material")});
    {
        Setting v = number("value", "Default", 0);
        v.size = 2;
        add("param_vector", "Parameter (vector)", "Parameters", "Two numbers each material sets for itself", {},
            {out("Vector", 2)},
            [](Writer& w) {
                std::string p = w.param(w.text("name"), 2, w.number("value"), 0, 0, false, w.text("help"));
                w.out(0, "vec4(" + p + ", 0.0, 0.0)", 2);
            },
            {text("name", "Name", "offset", "What it's called in the material and the code: letters, digits and _"), v,
             text("help", "Help", "", "What it's for, shown in the material")});
    }

    // Math
    binary("add", "Add", "A + B", all(0), all(0), "($a + $b)");
    binary("subtract", "Subtract", "A - B", all(0), all(0), "($a - $b)");
    binary("multiply", "Multiply", "A times B: a color times a number darkens it, two colors tint each other", all(1),
           all(1), "($a * $b)");
    binary("divide", "Divide", "A / B", all(1), all(1), "($a / $b)");
    binary("power", "Power", "A to the power of B", all(1), all(2), "pow(max($a, vec4(0.0)), $b)");
    binary("minimum", "Minimum", "The smaller of A and B", all(0), all(1), "min($a, $b)");
    binary("maximum", "Maximum", "The larger of A and B", all(0), all(1), "max($a, $b)");
    binary("modulo", "Modulo", "What's left of A after taking away B as many times as it goes", all(0), all(1),
           "mod($a, $b)");
    binary("step", "Step", "1 where B is at least A (the edge), otherwise 0", all(0.5f), all(0), "step($a, $b)");
    add("smoothstep", "Smooth step", "Math", "0 below the first edge, 1 above the second, smoothly between",
        {in("Edge 0", 1, all(0)), in("Edge 1", 1, all(1)), in("In", 1)}, {out("Out", 1)},
        [](Writer& w) { w.out(0, "smoothstep(" + w.in(0) + ", " + w.in(1) + ", " + w.in(2) + ")", w.size(2)); });
    add("clamp", "Clamp", "Math", "In, kept between Min and Max",
        {in("In", 1), in("Min", 1, all(0)), in("Max", 1, all(1))}, {out("Out", 1)},
        [](Writer& w) { w.out(0, "clamp(" + w.in(0) + ", " + w.in(1) + ", " + w.in(2) + ")", w.size(0)); });
    add("lerp", "Mix", "Math", "From A (T is 0) to B (T is 1)", {in("A", 1, all(0)), in("B", 1, all(1)), in("T", 1, all(0.5f))},
        {out("Out", 1)}, [](Writer& w) {
            w.out(0, "mix(" + w.in(0) + ", " + w.in(1) + ", " + w.in(2) + ")", std::max(w.size(0), w.size(1)));
        });
    add("remap", "Remap", "Math", "In, from one range to another",
        {in("In", 1), in("From min", 1, all(0)), in("From max", 1, all(1)), in("To min", 1, all(0)), in("To max", 1, all(1))},
        {out("Out", 1)}, [](Writer& w) {
            w.out(0,
                  "(" + w.in(3) + " + (" + w.in(0) + " - " + w.in(1) + ") * (" + w.in(4) + " - " + w.in(3) + ") / max(" +
                      w.in(2) + " - " + w.in(1) + ", vec4(0.00001)))",
                  w.size(0));
        });
    unary("one_minus", "One minus", "1 - In: turns a mask inside out", "(1.0 - $x)");
    unary("negate", "Negate", "-In", "(-$x)");
    unary("absolute", "Absolute", "In without its sign", "abs($x)");
    unary("floor", "Floor", "In rounded down", "floor($x)");
    unary("ceil", "Ceiling", "In rounded up", "ceil($x)");
    unary("round", "Round", "In rounded to the nearest whole number", "floor($x + 0.5)");
    unary("fraction", "Fraction", "What's after the point: repeats 0 to 1", "fract($x)");
    unary("sqrt", "Square root", "The square root of In", "sqrt(max($x, vec4(0.0)))");
    unary("sine", "Sine", "Waves between -1 and 1", "sin($x)");
    unary("cosine", "Cosine", "Waves between -1 and 1, a quarter turn after the sine", "cos($x)");
    unary("saturate", "Saturate", "In, kept between 0 and 1", "clamp($x, 0.0, 1.0)");
    unary("sign", "Sign", "-1, 0 or 1", "sign($x)");
    add("length", "Length", "Math", "How long a vector is", {in("In", 2)}, {out("Out", 1)},
        [](Writer& w) { w.out(0, "vec4(length(" + w.part(0) + "))", 1); });
    add("distance", "Distance", "Math", "How far apart A and B are", {in("A", 2), in("B", 2, {0.5f, 0.5f})}, {out("Out", 1)},
        [](Writer& w) {
            u8 n = w.widest();
            const char* sw[] = {"", ".x", ".xy", ".xyz", ""};
            w.out(0, "vec4(distance(" + w.in(0) + sw[n] + ", " + w.in(1) + sw[n] + "))", 1);
        });
    add("dot", "Dot product", "Math", "How much A and B point the same way", {in("A", 3), in("B", 3)}, {out("Out", 1)},
        [](Writer& w) {
            u8 n = w.widest();
            const char* sw[] = {"", ".x", ".xy", ".xyz", ""};
            w.out(0, "vec4(dot(" + w.in(0) + sw[n] + ", " + w.in(1) + sw[n] + "))", 1);
        });
    add("normalize", "Normalize", "Math", "The same direction, one long", {in("In", 2)}, {out("Out", 2)},
        [](Writer& w) {
            u8 n = w.size(0);
            std::string v = w.in(0);
            w.out(0,
                  n == 1   ? "vec4(normalize(" + v + ".x))"
                  : n == 2 ? "vec4(normalize(" + v + ".xy), 0.0, 0.0)"
                  : n == 3 ? "vec4(normalize(" + v + ".xyz), 0.0)"
                           : "normalize(" + v + ")",
                  n);
        });

    // Vectors
    add("split", "Split", "Vector", "A vector or color's parts on their own", {in("In", 4)},
        {out("X / R", 1), out("Y / G", 1), out("Z / B", 1), out("W / A", 1)}, [](Writer& w) {
            const char* parts[] = {"x", "y", "z", "w"};
            for (usize i = 0; i < 4; ++i) w.out(i, "vec4(" + w.in(0) + "." + parts[i] + ")", 1);
        });
    add("combine", "Combine", "Vector", "Numbers into a vector or color",
        {in("X / R", 1), in("Y / G", 1), in("Z / B", 1), in("W / A", 1, all(1))},
        {out("Color", 4, true), out("Vector", 2)}, [](Writer& w) {
            std::string c = w.local("v");
            w.line("vec4 " + c + " = vec4(" + w.in(0) + ".x, " + w.in(1) + ".x, " + w.in(2) + ".x, " + w.in(3) + ".x);");
            w.out(0, c, 4);
            w.out(1, c, 2);
        });

    // UV
    add("tiling_offset", "Tiling and offset", "UV", "Repeats a picture (tiling above 1) and slides it",
        {uv_in(), in("Tiling", 2, {1, 1}), in("Offset", 2, {0, 0})}, {out("UV", 2)},
        [](Writer& w) { w.out(0, "(" + w.in(0) + " * " + w.in(1) + " + " + w.in(2) + ")", 2); });
    add("rotate", "Rotate", "UV", "Turns UVs about a center, by degrees",
        {uv_in(), in("Center", 2, {0.5f, 0.5f}), in("Degrees", 1, all(0))}, {out("UV", 2)}, [](Writer& w) {
            w.need("rotate");
            w.out(0, "vec4(rotate_uv(" + w.in(0) + ".xy, " + w.in(1) + ".xy, radians(" + w.in(2) + ".x)), 0.0, 0.0)", 2);
        });
    add("panner", "Scroll", "UV", "Slides UVs over time: flowing water, drifting clouds",
        {uv_in(), in("Speed", 2, {0.1f, 0})}, {out("UV", 2)},
        [](Writer& w) { w.out(0, "(" + w.in(0) + " + " + w.in(1) + " * " + w.time() + ")", 2); });
    add("wave", "Wave", "UV", "Wobbles UVs: heat haze, water, a flag",
        {uv_in(), in("Amount", 1, all(0.01f)), in("Waves", 1, all(12)), in("Speed", 1, all(3))}, {out("UV", 2)},
        [](Writer& w) {
            std::string uv = w.in(0) + ".xy", t = w.time() + " * " + w.in(3) + ".x";
            w.out(0,
                  "vec4(" + uv + " + vec2(sin(" + w.in(0) + ".y * " + w.in(2) + ".x + " + t + "), cos(" + w.in(0) + ".x * " +
                      w.in(2) + ".x + " + t + ")) * " + w.in(1) + ".x, 0.0, 0.0)",
                  2);
        });
    add("polar", "Polar", "UV", "UVs as how far from a center and which way round (0 to 1)",
        {uv_in(), in("Center", 2, {0.5f, 0.5f})}, {out("UV", 2)}, [](Writer& w) {
            std::string d = w.local("d");
            w.line("vec2 " + d + " = " + w.in(0) + ".xy - " + w.in(1) + ".xy;");
            w.out(0, "vec4(length(" + d + "), atan(" + d + ".y, " + d + ".x) / 6.2831853 + 0.5, 0.0, 0.0)", 2);
        });
    add("pixelate", "Pixelate", "UV", "Snaps UVs to a grid of cells: chunky pixels", {uv_in(), in("Cells", 2, {16, 16})},
        {out("UV", 2)},
        [](Writer& w) { w.out(0, "((floor(" + w.in(0) + " * " + w.in(1) + ") + 0.5) / " + w.in(1) + ")", 2); });

    // Patterns
    add("noise", "Noise", "Pattern", "Smooth random values, 0 to 1. Cloudy adds finer layers on top",
        {uv_in(), in("Scale", 1, all(8))}, {out("Out", 1)},
        [](Writer& w) {
            std::string p = w.in(0) + ".xy * " + w.in(1) + ".x";
            if (w.text("kind") == "cloudy") {
                w.need("fbm");
                w.out(0, "vec4(fbm(" + p + ", " + std::to_string(std::clamp(static_cast<int>(w.num("layers")), 1, 8)) + "))", 1);
            } else {
                w.need("noise");
                w.out(0, "vec4(value_noise(" + p + "))", 1);
            }
        },
        {choice("kind", "Kind", {"smooth", "cloudy"}), number("layers", "Layers", 4, 1, 8, "Cloudy noise's layers")});
    add("voronoi", "Cells", "Pattern", "Voronoi cells: how far to the nearest cell's middle, and a random value for each cell",
        {uv_in(), in("Scale", 1, all(6))}, {out("Distance", 1), out("Cell", 1)}, [](Writer& w) {
            w.need("voronoi");
            std::string v = w.local("v");
            w.line("vec2 " + v + " = voronoi(" + w.in(0) + ".xy * " + w.in(1) + ".x);");
            w.out(0, "vec4(" + v + ".x)", 1);
            w.out(1, "vec4(" + v + ".y)", 1);
        });
    add("random", "Random", "Pattern", "A random value, 0 to 1, for each different input", {uv_in("Seed")}, {out("Out", 1)},
        [](Writer& w) {
            w.need("hash");
            w.out(0, "vec4(hash12(" + w.in(0) + ".xy * 1000.0))", 1);
        });
    add("checker", "Checkerboard", "Pattern", "Squares of 0 and 1", {uv_in(), in("Squares", 2, {8, 8})}, {out("Out", 1)},
        [](Writer& w) {
            std::string c = "floor(" + w.in(0) + ".xy * " + w.in(1) + ".xy)";
            w.out(0, "vec4(mod(" + c + ".x + " + c + ".y, 2.0))", 1);
        });
    add("gradient", "Gradient", "Pattern", "0 to 1 across, down, out from the middle, or round it", {uv_in()}, {out("Out", 1)},
        [](Writer& w) {
            std::string uv = w.in(0), k = w.text("kind");
            if (k == "down") w.out(0, "vec4(" + uv + ".y)", 1);
            else if (k == "out from the middle") w.out(0, "vec4(clamp(length(" + uv + ".xy - 0.5) * 2.0, 0.0, 1.0))", 1);
            else if (k == "round") w.out(0, "vec4(atan(" + uv + ".y - 0.5, " + uv + ".x - 0.5) / 6.2831853 + 0.5)", 1);
            else w.out(0, "vec4(" + uv + ".x)", 1);
        },
        {choice("kind", "Kind", {"across", "down", "out from the middle", "round"})});
    add("circle", "Circle", "Pattern", "1 inside a circle, 0 outside, with a soft edge",
        {uv_in(), in("Center", 2, {0.5f, 0.5f}), in("Radius", 1, all(0.4f)), in("Softness", 1, all(0.02f))}, {out("Out", 1)},
        [](Writer& w) {
            std::string d = "distance(" + w.in(0) + ".xy, " + w.in(1) + ".xy)", r = w.in(2) + ".x", s = w.in(3) + ".x";
            w.out(0, "vec4(1.0 - smoothstep(" + r + " - " + s + ", " + r + " + " + s + ", " + d + "))", 1);
        });
    add("stripes", "Stripes", "Pattern", "Bands of 0 and 1, at an angle", {uv_in(), in("Stripes", 1, all(8)), in("Degrees", 1, all(0))},
        {out("Out", 1)}, [](Writer& w) {
            std::string a = "radians(" + w.in(2) + ".x)";
            w.out(0, "vec4(step(0.5, fract(dot(" + w.in(0) + ".xy, vec2(cos(" + a + "), sin(" + a + "))) * " + w.in(1) + ".x)))", 1);
        });

    // Color
    add("hsv", "Hue, saturation, brightness", "Color", "Shifts a color's hue (degrees) and scales its saturation and brightness",
        {in("Color", 4, all(1), true), in("Hue", 1, all(0)), in("Saturation", 1, all(1)), in("Brightness", 1, all(1))},
        {out("Color", 4, true)}, [](Writer& w) {
            w.need("hsv");
            std::string h = w.local("h");
            w.line("vec3 " + h + " = rgb_to_hsv(" + w.in(0) + ".rgb);");
            w.out(0,
                  "vec4(hsv_to_rgb(vec3(fract(" + h + ".x + " + w.in(1) + ".x / 360.0), clamp(" + h + ".y * " + w.in(2) +
                      ".x, 0.0, 1.0), " + h + ".z * " + w.in(3) + ".x)), " + w.in(0) + ".a)",
                  4);
        });
    add("luminance", "Brightness", "Color", "How bright a color looks, 0 to 1", {in("Color", 3, all(1), true)}, {out("Out", 1)},
        [](Writer& w) { w.out(0, "vec4(dot(" + w.in(0) + ".rgb, vec3(0.299, 0.587, 0.114)))", 1); });
    add("desaturate", "Desaturate", "Color", "Toward grey", {in("Color", 4, all(1), true), in("Amount", 1, all(1))},
        {out("Color", 4, true)}, [](Writer& w) {
            std::string c = w.in(0);
            w.out(0, "vec4(mix(" + c + ".rgb, vec3(dot(" + c + ".rgb, vec3(0.299, 0.587, 0.114))), " + w.in(1) + ".x), " + c + ".a)", w.size(0));
        });
    add("posterize", "Posterize", "Color", "Fewer shades of each color", {in("Color", 4, all(1), true), in("Steps", 1, all(4))},
        {out("Color", 4, true)}, [](Writer& w) {
            std::string c = w.in(0), s = "max(" + w.in(1) + ".x, 1.0)";
            w.out(0, "vec4(floor(" + c + ".rgb * " + s + " + 0.5) / " + s + ", " + c + ".a)", w.size(0));
        });
    add("contrast", "Contrast", "Color", "Darks darker and lights lighter (above 1), or flatter (below)",
        {in("Color", 4, all(1), true), in("Amount", 1, all(1.5f))}, {out("Color", 4, true)}, [](Writer& w) {
            std::string c = w.in(0);
            w.out(0, "vec4((" + c + ".rgb - 0.5) * " + w.in(1) + ".x + 0.5, " + c + ".a)", w.size(0));
        });
    add("rainbow", "Rainbow", "Color", "A color round the color wheel: 0 red, through every hue, to 1 red again",
        {in("Hue", 1, all(0))}, {out("Color", 4, true)}, [](Writer& w) {
            w.need("hsv");
            w.out(0, "vec4(hsv_to_rgb(vec3(fract(" + w.in(0) + ".x), 1.0, 1.0)), 1.0)", 4);
        });
    add("blend", "Blend", "Color", "One color over another, the way image editors' layers blend",
        {in("Base", 4, all(1), true), in("Top", 4, all(1), true), in("Opacity", 1, all(1))}, {out("Color", 4, true)},
        [](Writer& w) {
            std::string b = w.in(0) + ".rgb", t = w.in(1) + ".rgb", mode = w.text("mode"), m;
            if (mode == "multiply") m = b + " * " + t;
            else if (mode == "screen") m = "1.0 - (1.0 - " + b + ") * (1.0 - " + t + ")";
            else if (mode == "add") m = b + " + " + t;
            else if (mode == "overlay")
                m = "mix(2.0 * " + b + " * " + t + ", 1.0 - 2.0 * (1.0 - " + b + ") * (1.0 - " + t + "), step(0.5, " + b + "))";
            else m = t;
            w.out(0, "vec4(mix(" + b + ", " + m + ", " + w.in(2) + ".x * " + w.in(1) + ".a), " + w.in(0) + ".a)", 4);
        },
        {choice("mode", "Mode", {"normal", "multiply", "screen", "add", "overlay"})});

    // Effects
    add("outline", "Outline", "Effect",
        "1 just outside the sprite's shape (where its picture is see-through but a neighbor isn't): mix in a color with it",
        {uv_in(), in("Pixels", 1, all(1))}, {out("Outline", 1)}, [](Writer& w) {
            std::string s = w.sampler("u_texture"), px = w.local("px"), uv = w.local("uv"), a = w.local("a");
            w.line("vec2 " + px + " = " + w.in(1) + ".x / vec2(textureSize(" + s + ", 0));");
            w.line("vec2 " + uv + " = " + w.in(0) + ".xy;");
            w.line("float " + a + " = max(max(texture(" + s + ", " + uv + " + vec2(" + px + ".x, 0.0)).a, texture(" + s + ", " + uv +
                   " - vec2(" + px + ".x, 0.0)).a), max(texture(" + s + ", " + uv + " + vec2(0.0, " + px + ".y)).a, texture(" + s +
                   ", " + uv + " - vec2(0.0, " + px + ".y)).a));");
            w.out(0, "vec4(" + a + " * (1.0 - texture(" + s + ", " + uv + ").a))", 1);
        });
    add("dissolve", "Dissolve", "Effect",
        "Burns a color away where Noise is below Amount, with a glowing edge: a spell, a teleport, something destroyed",
        {in("Color", 4, all(1), true), in("Noise", 1, all(0.5f)), in("Amount", 1, all(0.5f)), in("Edge", 1, all(0.06f)),
         in("Edge color", 4, {1, 0.45f, 0.1f, 1}, true)},
        {out("Color", 4, true)}, [](Writer& w) {
            std::string n = w.local("n"), a = w.local("a"), keep = w.local("keep"), rim = w.local("rim");
            w.line("float " + n + " = " + w.in(1) + ".x, " + a + " = " + w.in(2) + ".x;");
            w.line("float " + keep + " = step(" + a + ", " + n + ");");
            w.line("float " + rim + " = " + keep + " * (1.0 - smoothstep(" + a + ", " + a + " + max(" + w.in(3) +
                   ".x, 0.0001), " + n + ")) * step(0.0001, " + a + ");");
            w.out(0, "vec4(mix(" + w.in(0) + ".rgb, " + w.in(4) + ".rgb * 3.0, " + rim + "), " + w.in(0) + ".a * " + keep + ")", 4);
        });
    add("flicker", "Flicker", "Effect", "A light's unsteady brightness over time: mostly 1, dipping now and then",
        {in("Speed", 1, all(8)), in("Depth", 1, all(0.35f))}, {out("Out", 1)}, [](Writer& w) {
            w.need("noise");
            w.out(0, "vec4(1.0 - " + w.in(1) + ".x * value_noise(vec2(" + w.time() + " * " + w.in(0) + ".x, 0.5)))", 1);
        });

    // The end of every graph
    add("output", "Output", "Output",
        "What the sprite shows: its color and how see-through it is. Glow adds brightness that blooms",
        {in("Color", 4, all(1), true), in("Alpha", 1, all(1)), in("Glow", 3, all(0), true)}, {}, nullptr);
    return types;
}

bool valid_name(std::string_view n) {
    if (n.empty() || n.size() > 48 || std::isdigit(static_cast<unsigned char>(n[0]))) return false;
    for (char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    static const std::set<std::string_view> reserved = {
        "main", "texture", "in", "out", "uniform", "float", "int", "vec2", "vec3", "vec4", "if", "else", "for", "return",
        "v_uv", "v_color", "out_color", "Params", "mix", "step", "sin", "cos", "length", "dot", "time", "layout", "const",
        "discard", "bool", "true", "false", "while", "do", "break", "continue", "struct", "void", "sampler2D", "smooth"};
    return !reserved.contains(n) && !n.starts_with("gl_") && !n.starts_with("bowl_") && !n.starts_with("n_");
}

} // namespace

const std::vector<NodeType>& node_types() {
    static const std::vector<NodeType> types = make_types();
    return types;
}

const NodeType* node_type(std::string_view id) {
    for (const NodeType& t : node_types())
        if (t.id == id) return &t;
    return nullptr;
}

// --- Graph ----------------------------------------------------------------------------------------------------------

Node* Graph::node(u32 id) {
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Node* Graph::node(u32 id) const { return const_cast<Graph*>(this)->node(id); }

u32 Graph::add(std::string_view type, Vec2 at) {
    if (!node_type(type)) return 0;
    Node n;
    n.id = next_id++;
    n.type = std::string(type);
    n.position = at;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void Graph::remove(u32 id) {
    const Node* n = node(id);
    if (!n || n->type == "output") return;
    std::erase_if(links, [&](const Link& l) { return l.from == id || l.to == id; });
    std::erase_if(nodes, [&](const Node& x) { return x.id == id; });
}

bool Graph::feeds(u32 from, u32 to) const {
    // Does anything flowing out of `to` reach `from`?
    std::vector<u32> stack{to};
    std::set<u32> seen;
    while (!stack.empty()) {
        u32 at = stack.back();
        stack.pop_back();
        if (at == from) return true;
        if (!seen.insert(at).second) continue;
        for (const Link& l : links)
            if (l.from == at) stack.push_back(l.to);
    }
    return false;
}

bool Graph::connect(u32 from, u32 output, u32 to, u32 input) {
    const Node* a = node(from);
    const Node* b = node(to);
    if (!a || !b || from == to) return false;
    const NodeType* ta = node_type(a->type);
    const NodeType* tb = node_type(b->type);
    if (!ta || !tb || output >= ta->outputs.size() || input >= tb->inputs.size()) return false;
    if (feeds(from, to)) return false;
    disconnect(to, input);
    links.push_back({from, output, to, input});
    return true;
}

void Graph::disconnect(u32 to, u32 input) {
    std::erase_if(links, [&](const Link& l) { return l.to == to && l.input == input; });
}

const Link* Graph::link_into(u32 to, u32 input) const {
    for (const Link& l : links)
        if (l.to == to && l.input == input) return &l;
    return nullptr;
}

u32 Graph::output_node() const {
    for (const Node& n : nodes)
        if (n.type == "output") return n.id;
    return 0;
}

Values Graph::number(const Node& n, std::string_view key) const {
    if (auto it = n.numbers.find(key); it != n.numbers.end()) return it->second;
    const NodeType* t = node_type(n.type);
    if (!t) return {};
    if (key.starts_with("in:")) {
        for (const Pin& p : t->inputs)
            if (p.name == key.substr(3)) return p.value;
        return {};
    }
    for (const Setting& s : t->settings)
        if (s.key == key) return s.value;
    return {};
}

std::string Graph::text(const Node& n, std::string_view key) const {
    if (auto it = n.texts.find(key); it != n.texts.end()) return it->second;
    if (const NodeType* t = node_type(n.type))
        for (const Setting& s : t->settings)
            if (s.key == key) return s.text;
    return {};
}

Graph starter() {
    Graph g;
    u32 tex = g.add("texture", {40, 80});
    u32 tint = g.add("sprite_color", {40, 260});
    u32 mul = g.add("multiply", {300, 140});
    u32 out = g.add("output", {540, 120});
    g.connect(tex, 0, mul, 0);
    g.connect(tint, 0, mul, 1);
    g.connect(mul, 0, out, 0);
    return g;
}

json::Value to_json(const Graph& g) {
    json::Value doc;
    doc.set("bowl_shadergraph", 1);
    json::Value& nodes = doc.set("nodes", json::Array{});
    for (const Node& n : g.nodes) {
        json::Value o;
        o.set("id", n.id);
        o.set("type", n.type);
        o.set("x", std::round(n.position.x));
        o.set("y", std::round(n.position.y));
        const NodeType* t = node_type(n.type);
        if (!n.numbers.empty()) {
            json::Value& values = o.set("values", json::Object{});
            for (const auto& [key, v] : n.numbers) {
                u8 size = 4;
                if (t && key.starts_with("in:")) {
                    for (const Pin& p : t->inputs)
                        if (p.name == std::string_view(key).substr(3)) size = p.size;
                } else if (t) {
                    for (const Setting& s : t->settings)
                        if (s.key == key) size = s.kind == Setting::Kind::Color ? 4 : s.size;
                }
                json::Value a;
                for (u8 i = 0; i < size; ++i) a.push(v[i]);
                values.set(key, std::move(a));
            }
        }
        if (!n.texts.empty()) {
            json::Value& texts = o.set("texts", json::Object{});
            for (const auto& [key, v] : n.texts) texts.set(key, v);
        }
        nodes.push(std::move(o));
    }
    json::Value& links = doc.set("links", json::Array{});
    for (const Link& l : g.links) {
        const Node* a = g.node(l.from);
        const Node* b = g.node(l.to);
        const NodeType* ta = a ? node_type(a->type) : nullptr;
        const NodeType* tb = b ? node_type(b->type) : nullptr;
        if (!ta || !tb) continue;
        json::Value o;
        o.set("from", l.from);
        o.set("output", ta->outputs[l.output].name);
        o.set("to", l.to);
        o.set("input", tb->inputs[l.input].name);
        links.push(std::move(o));
    }
    return doc;
}

std::optional<Graph> from_json(const json::Value& v, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<Graph> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!v.is_object() || !v["nodes"].is_array()) return fail("not a shader graph: it needs a list of nodes");
    Graph g;
    for (const json::Value& o : v["nodes"].array()) {
        Node n;
        n.id = static_cast<u32>(o["id"].as_int());
        n.type = std::string(o["type"].as_string());
        n.position = {o["x"].as_float(), o["y"].as_float()};
        if (n.id == 0 || g.node(n.id)) return fail(std::format("a node without an id of its own ({})", n.id));
        if (!node_type(n.type)) return fail(std::format("no kind of node is called '{}'", n.type));
        for (const auto& [key, value] : o["values"].object()) {
            Values numbers{};
            if (value.is_number()) numbers[0] = value.as_float();
            for (usize i = 0; i < 4 && i < value.size(); ++i) numbers[i] = value[i].as_float();
            n.numbers[key] = numbers;
        }
        for (const auto& [key, value] : o["texts"].object()) n.texts[key] = std::string(value.as_string());
        g.next_id = std::max(g.next_id, n.id + 1);
        g.nodes.push_back(std::move(n));
    }
    for (const json::Value& o : v["links"].array()) {
        u32 from = static_cast<u32>(o["from"].as_int()), to = static_cast<u32>(o["to"].as_int());
        const Node* a = g.node(from);
        const Node* b = g.node(to);
        if (!a || !b) continue;
        const NodeType* ta = node_type(a->type);
        const NodeType* tb = node_type(b->type);
        auto index = [](const std::vector<Pin>& pins, std::string_view name) -> std::optional<u32> {
            for (u32 i = 0; i < pins.size(); ++i)
                if (pins[i].name == name) return i;
            return std::nullopt;
        };
        auto out = index(ta->outputs, o["output"].as_string());
        auto in = index(tb->inputs, o["input"].as_string());
        if (out && in) g.connect(from, *out, to, *in);
    }
    if (!g.output_node()) g.add("output", {600, 120});
    return g;
}

// --- Writing GLSL ---------------------------------------------------------------------------------------------------

struct Writer::State {
    std::set<std::string> helpers;
    bool time = false, screen = false;
    std::vector<std::string> samplers; // extra textures, in binding order (1, 2, 3)
    bool sprite_texture = false;
    struct Param {
        std::string name;
        u8 size;
        Values value;
        f32 min, max;
        bool color;
        std::string help;
    };
    std::vector<Param> params;
    std::vector<std::string> problems;
    u32 node = 0;
};

std::string Writer::literal(f32 v) {
    if (!std::isfinite(v)) v = 0;
    std::string s = std::format("{:.6g}", v);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string Writer::vec4(const Values& v, u8 size) {
    Values x = v;
    for (u8 i = size; i < 4; ++i) x[i] = size == 1 ? v[0] : 0.0f;
    return "vec4(" + literal(x[0]) + ", " + literal(x[1]) + ", " + literal(x[2]) + ", " + literal(x[3]) + ")";
}

std::string Writer::in(usize i) const { return i < in_.size() ? in_[i] : std::string("vec4(0.0)"); }
u8 Writer::size(usize i) const { return i < sizes_.size() ? sizes_[i] : 1; }
u8 Writer::widest() const {
    u8 w = 1;
    for (u8 s : sizes_) w = std::max(w, s);
    return w;
}
std::string Writer::part(usize i) const {
    switch (size(i)) {
    case 1: return in(i) + ".x";
    case 2: return in(i) + ".xy";
    case 3: return in(i) + ".xyz";
    default: return in(i);
    }
}
Values Writer::number(std::string_view key) const { return graph_->number(*node_, key); }
std::string Writer::text(std::string_view key) const { return graph_->text(*node_, key); }
void Writer::out(usize i, const std::string& expr, u8 size) {
    if (out_.size() <= i) out_.resize(i + 1, {"vec4(0.0)", 1});
    out_[i] = {expr, size};
}
void Writer::line(const std::string& code) { lines_.push_back(code); }
std::string Writer::local(std::string_view part) const { return std::format("n{}_{}", node_->id, part); }
void Writer::need(std::string_view helper) {
    std::vector<std::string_view> stack{helper};
    while (!stack.empty()) {
        std::string_view h = stack.back();
        stack.pop_back();
        if (!state_->helpers.insert(std::string(h)).second) continue;
        for (const Helper& x : helpers())
            if (x.name == h)
                for (const char* d : x.needs) stack.push_back(d);
    }
}
std::string Writer::time() {
    state_->time = true;
    return "bowl_time";
}
std::string Writer::screen() {
    state_->screen = true;
    return "bowl_screen";
}
std::string Writer::sampler(std::string_view name) {
    if (name.empty() || name == "u_texture") {
        state_->sprite_texture = true;
        return "u_texture";
    }
    std::string n(name);
    if (!valid_name(n)) {
        state_->problems.push_back(std::format("'{}' won't do as a texture's name: letters, digits and _", n));
        state_->sprite_texture = true;
        return "u_texture";
    }
    if (std::find(state_->samplers.begin(), state_->samplers.end(), n) != state_->samplers.end()) return n;
    if (state_->samplers.size() >= 3) {
        state_->problems.push_back(std::format("A material has three extra textures at most: '{}' is a fourth", n));
        state_->sprite_texture = true;
        return "u_texture";
    }
    state_->samplers.push_back(n);
    return n;
}
std::string Writer::param(std::string name, u8 size, Values value, f32 min, f32 max, bool color, std::string help) {
    if (!valid_name(name)) {
        state_->problems.push_back(std::format("'{}' won't do as a parameter's name: letters, digits and _, not starting "
                                               "with a digit or bowl_", name));
        name = std::format("param{}", node_->id);
    }
    for (const State::Param& p : state_->params)
        if (p.name == name) {
            if (p.size != size || p.color != color)
                state_->problems.push_back(std::format("Two parameters are called '{}'", name));
            return name;
        }
    for (const std::string& s : state_->samplers)
        if (s == name) state_->problems.push_back(std::format("'{}' names a texture and a parameter", name));
    std::erase(help, '\n');
    state_->params.push_back({name, size, value, min, max, color, help});
    return name;
}

Generated generate(const Graph& g, std::string_view source) {
    Generated result;
    Writer::State state;
    u32 out_id = g.output_node();

    // The nodes the output needs, each after what it needs.
    std::vector<u32> order;
    std::set<u32> done, visiting;
    std::function<void(u32)> visit = [&](u32 id) {
        if (done.contains(id) || visiting.contains(id)) return;
        visiting.insert(id);
        const Node* n = g.node(id);
        const NodeType* t = n ? node_type(n->type) : nullptr;
        if (t)
            for (u32 i = 0; i < t->inputs.size(); ++i)
                if (const Link* l = g.link_into(id, i)) visit(l->from);
        visiting.erase(id);
        done.insert(id);
        order.push_back(id);
    };
    if (out_id) visit(out_id);

    std::map<std::pair<u32, u32>, u8> sizes; // each output's size
    std::vector<std::string> body;
    auto inputs = [&](const Node& n, const NodeType& t, std::vector<std::string>& exprs, std::vector<u8>& widths) {
        for (u32 i = 0; i < t.inputs.size(); ++i) {
            const Pin& p = t.inputs[i];
            if (const Link* l = g.link_into(n.id, i)) {
                exprs.push_back(std::format("n{}_{}", l->from, l->output));
                auto it = sizes.find({l->from, l->output});
                widths.push_back(it == sizes.end() ? 4 : it->second);
            } else if (p.uv) {
                exprs.push_back("vec4(v_uv, 0.0, 0.0)");
                widths.push_back(2);
            } else {
                exprs.push_back(Writer::vec4(g.number(n, "in:" + p.name), p.size));
                widths.push_back(p.size);
            }
        }
    };
    for (u32 id : order) {
        if (id == out_id) continue;
        const Node& n = *g.node(id);
        const NodeType& t = *node_type(n.type);
        Writer w;
        w.state_ = &state;
        w.graph_ = &g;
        w.node_ = &n;
        inputs(n, t, w.in_, w.sizes_);
        if (t.write) t.write(w);
        body.push_back(std::format("    // {}", t.title));
        for (const std::string& l : w.lines_) body.push_back("    " + l);
        for (u32 i = 0; i < t.outputs.size(); ++i) {
            auto [expr, size] = i < w.out_.size() ? w.out_[i] : std::pair<std::string, u8>{"vec4(0.0)", 1};
            body.push_back(std::format("    vec4 n{}_{} = {};", id, i, masked(expr, size)));
            sizes[{id, i}] = size;
        }
    }
    // The output: a color (alpha 1 if it has none), times Alpha, plus Glow, premultiplied.
    std::string color = "vec4(1.0)", alpha = "vec4(1.0)", glow = "vec4(0.0)";
    bool color_has_alpha = true;
    if (const Node* o = g.node(out_id)) {
        const NodeType& t = *node_type(o->type);
        std::vector<std::string> exprs;
        std::vector<u8> widths;
        inputs(*o, t, exprs, widths);
        color = exprs[0];
        alpha = exprs[1];
        glow = exprs[2];
        color_has_alpha = widths[0] == 4;
    }
    body.push_back("    // Output");
    body.push_back("    vec4 color = " + color + ";");
    body.push_back(std::string("    float alpha = clamp(") + (color_has_alpha ? "color.a" : "1.0") + " * " + alpha + ".x, 0.0, 1.0);");
    body.push_back("    out_color = vec4((color.rgb + " + glow + ".rgb) * alpha, alpha);");

    // The file
    std::string s;
    s += "#version 450\n";
    s += std::format("// Made by Bowl Editor's shader graph from {}: change the graph, not this file.\n\n", source);
    s += "layout(location = 0) in vec2 v_uv;\n";
    s += "layout(location = 1) in vec4 v_color; // premultiplied\n";
    s += "layout(location = 0) out vec4 out_color;\n\n";
    // The sprite's texture is always binding 0, whether the graph reads it or not.
    s += "layout(set = 2, binding = 0) uniform sampler2D u_texture;\n";
    for (usize i = 0; i < state.samplers.size(); ++i)
        s += std::format("layout(set = 2, binding = {}) uniform sampler2D {};\n", i + 1, state.samplers[i]);
    if (!state.params.empty() || state.time || state.screen) {
        s += "layout(std140, set = 3, binding = 0) uniform Params {\n";
        for (const Writer::State::Param& p : state.params) {
            const char* type = p.size == 1 ? "float" : p.size == 2 ? "vec2" : p.size == 3 ? "vec3" : "vec4";
            std::string comment = p.color ? "color = " : "= ";
            for (u8 i = 0; i < p.size; ++i) comment += (i ? ", " : "") + Writer::literal(p.value[i]);
            if (p.max > p.min) comment += " [" + Writer::literal(p.min) + ", " + Writer::literal(p.max) + "]";
            if (!p.help.empty()) comment += "  " + p.help;
            s += std::format("    {} {}; // {}\n", type, p.name, comment);
        }
        if (state.time) s += "    float bowl_time; // seconds: the renderer fills it in\n";
        if (state.screen) s += "    vec4 bowl_screen; // the target's size, and one over it: the renderer fills it in\n";
        s += "};\n";
    }
    s += "\n";
    for (const Helper& h : helpers())
        if (state.helpers.contains(h.name)) s += std::string(h.code) + "\n\n";
    s += "void main() {\n";
    for (const std::string& l : body) s += l + "\n";
    // Keep u_texture in the shader even when the graph doesn't read it, so the sprite texture stays binding 0.
    if (!state.sprite_texture) s += "    out_color *= min(1.0, texture(u_texture, v_uv).a + 1.0);\n";
    s += "}\n";
    result.glsl = std::move(s);
    result.problems = std::move(state.problems);
    return result;
}

} // namespace bowl::shadergraph
