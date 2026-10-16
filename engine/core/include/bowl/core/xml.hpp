#pragma once

#include "bowl/core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bowl::xml {

// One element of an XML document. Enough XML for tool formats like Tiled's .tmx: elements, attributes, text,
// comments, CDATA and the standard entities. No namespaces, DTDs or validation.
struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes; // in file order, entities decoded
    std::vector<Element> children;
    std::string text; // character data directly inside this element, joined

    std::string_view attribute(std::string_view key, std::string_view fallback = {}) const;
    bool has(std::string_view key) const;
    // The first child with this name, or null.
    const Element* child(std::string_view child_name) const;
};

struct ParseError {
    std::string message;
    u32 line = 0, column = 0;
};

struct ParseResult {
    Element root;
    std::optional<ParseError> error;
    explicit operator bool() const { return !error; }
};

ParseResult parse(std::string_view text);

} // namespace bowl::xml
