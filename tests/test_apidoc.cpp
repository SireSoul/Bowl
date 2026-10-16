#include "bowl/apidoc/apidoc.hpp"

#include <doctest/doctest.h>

using namespace bowl;
using apidoc::Item;

namespace {

constexpr std::string_view sample = R"(#pragma once

#include "bowl/core/types.hpp"

// Widgets for testing. Use them like this:
//
//     Widget w;
//     w.spin(3);

namespace bowl::demo {

class Engine; // forward, left out

// How a widget turns.
enum class Spin : u8 {
    Left,
    Right, // clockwise
};

// Plain settings.
struct Settings {
    i32 speed = 3;     // turns a second
    Vec2 axis{0, 1};
    // How sticky.
    f32 friction = 0.5f;
};

// Spins things.
class Widget : NonCopyable {
public:
    Widget(i32 speed) : speed_(speed), other_(0) {}
    // Turns it `times` times. Returns Vector<T> of angles.
    std::vector<f32> spin(i32 times);
    i32 speed() const { return speed_; }
    bool spinning() const;

    // Raw access
    i32* data();

private:
    i32 speed_ = 0, other_ = 0;
    void secret();
};

namespace detail {
inline int hidden() { return 1; }
} // namespace detail

// Free function.
template <class T> T twice(T v) {
    return v + v;
}

std::vector<f32> Widget::spin(i32 times) {
    return {};
}

// Reflected.
BOWL_REFLECT(Settings, speed, axis,
           friction)

#define BOWL_DEMO(x) do_it(x) // a macro

} // namespace bowl::demo
)";

} // namespace

TEST_CASE("apidoc: headers become comments and declarations") {
    apidoc::Header h = apidoc::parse("bowl/demo/widget.hpp", sample);
    CHECK(h.path == "bowl/demo/widget.hpp");
    CHECK(h.namespaces == std::vector<std::string>{"bowl::demo"});
    REQUIRE(h.intro.size() == 4);
    CHECK(h.intro[0] == "Widgets for testing. Use them like this:");
    CHECK(h.intro[2] == "    Widget w;");

    std::vector<std::string> codes;
    for (const Item& item : h.items) codes.push_back(item.kind == Item::Kind::Class ? "class " + item.name : item.code);
    CHECK(codes == std::vector<std::string>{"enum class Spin : u8 {\n    Left,\n    Right, // clockwise\n};", "class Settings",
                                            "class Widget", "template <class T> T twice(T v);",
                                            "BOWL_REFLECT(Settings, speed, axis,\n           friction)", "#define BOWL_DEMO(x) do_it(x) // a macro"});
    CHECK(h.items[0].comment == std::vector<std::string>{"How a widget turns."});

    const Item& settings = h.items[1];
    REQUIRE(settings.members.size() == 3);
    CHECK(settings.members[0].code == "i32 speed = 3;");
    CHECK(settings.members[0].trailing == "turns a second");
    CHECK(settings.members[1].code == "Vec2 axis{0, 1};");
    CHECK(settings.members[2].comment == std::vector<std::string>{"How sticky."});

    const Item& widget = h.items[2];
    CHECK(widget.code == "class Widget : NonCopyable");
    CHECK(widget.comment == std::vector<std::string>{"Spins things."});
    std::vector<std::string> members;
    for (const Item& m : widget.members) members.push_back(m.kind == Item::Kind::Note ? "note" : m.code);
    CHECK(members == std::vector<std::string>{"Widget(i32 speed);", "std::vector<f32> spin(i32 times);", "i32 speed() const;",
                                              "bool spinning() const;", "i32* data();"});
}

TEST_CASE("apidoc: Markdown pages") {
    std::vector<apidoc::Header> headers{apidoc::parse("bowl/demo/widget.hpp", sample)};
    std::string page = apidoc::render_module("demo", headers);
    CHECK(page.starts_with("# demo\n"));
    CHECK(page.find("- [bowl/demo/widget.hpp](#bowldemowidgethpp)") != std::string::npos);
    CHECK(page.find("```cpp\nWidget w;\nw.spin(3);\n```") != std::string::npos);    // the intro's example
    CHECK(page.find("Returns Vector&lt;T> of angles.") != std::string::npos);        // no stray HTML tags
    CHECK(page.find("`times`") != std::string::npos);
    // Plain structs whole, with their comments.
    CHECK(page.find("```cpp\nstruct Settings {\n    i32 speed = 3;     // turns a second\n    Vec2 axis{0, 1};\n    // How sticky.\n"
                    "    f32 friction = 0.5f;\n};\n```") != std::string::npos);
    // Undocumented declarations right after a documented one share its block; a blank line starts a new one.
    CHECK(page.find("```cpp\nstd::vector<f32> spin(i32 times);\ni32 speed() const;\nbool spinning() const;\n```") != std::string::npos);
    CHECK(page.find("Raw access\n\n```cpp\ni32* data();\n```") != std::string::npos);
    CHECK(page.find("secret") == std::string::npos); // private
    CHECK(page.find("hidden") == std::string::npos); // detail
    CHECK(page.find("Engine;") == std::string::npos);
    CHECK(page.find("return v + v") == std::string::npos);

    std::string index = apidoc::render_index({{"demo", headers}});
    CHECK(index.find("| [bowl/demo/widget.hpp](demo.md#bowldemowidgethpp) | Widgets for testing. |") !=
          std::string::npos);
    CHECK(apidoc::anchor("bowl/net/host.hpp") == "bowlnethosthpp");
    CHECK(apidoc::anchor("Two Words") == "two-words");
}
