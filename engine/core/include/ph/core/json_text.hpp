#pragma once

#include "ph/core/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ph::json {

// A JSON document edited as text: each change rewrites only the value it touches, so comments, spacing, key order and
// trailing commas stay as the person who wrote the file left them. For tools that edit files people also edit by hand.
//
//     TextDocument doc;
//     doc.load(*read_file_text("data/enemies.json"));
//     doc.set({"slime", "hp"}, 40);
//     doc.insert({"slime", "drops"}, "gel");
//     write_file_bytes(path, doc.text().data(), doc.text().size());
class TextDocument {
public:
    // A key in an object or an index in an array, from the root down.
    using Step = std::variant<std::string, usize>;
    using Path = std::vector<Step>;

    // False (with the parse error) if the text isn't valid JSON; the text is kept either way so it can be fixed.
    bool load(std::string text);
    const std::string& text() const { return text_; }
    const Value& value() const { return value_; }
    bool valid() const { return !error_; }
    const std::optional<ParseError>& error() const { return error_; }

    // Replaces the value at an existing path.
    bool set(const Path& path, const Value& value);
    // Appends to the array at `container`, or adds `key` to the object there (replacing it if it exists).
    bool insert(const Path& container, const Value& value, std::string_view key = {});
    // Removes an item or a key and its value, with the comma that went with it.
    bool erase(const Path& path);

private:
    struct Node {
        usize start = 0, end = 0;  // the value's text
        usize key_start = 0;       // members: where the key's quote starts (else = start)
        char kind = 0;             // '{', '[', or 0 for anything else
        usize open = 0, close = 0; // containers: positions of the brackets
        std::vector<std::string> keys;
        std::vector<Node> children;
    };
    bool scan();
    const Node* find(const Path& path) const;
    std::string format(const Value& value, usize at) const; // pretty for containers, indented to fit at `at`
    std::string indent_at(usize at) const;
    bool replace(usize start, usize end, std::string_view with);

    std::string text_;
    Value value_;
    std::optional<ParseError> error_;
    Node root_;
};

} // namespace ph::json
