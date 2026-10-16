#include "bowl/core/json_text.hpp"

namespace bowl::json {

namespace {

// Finds where each value sits in the text. The text is already known to parse, so this only has to track positions.
class Scanner {
public:
    explicit Scanner(std::string_view s) : s_(s) {}

    template <class NodeT> bool value(NodeT& n, int depth) {
        if (depth > 512) return false;
        skip();
        if (at_ >= s_.size()) return false;
        n.start = n.key_start = at_;
        char c = s_[at_];
        if (c == '{' || c == '[') {
            n.kind = c;
            n.open = at_++;
            char close = c == '{' ? '}' : ']';
            for (;;) {
                skip();
                if (at_ >= s_.size()) return false;
                if (s_[at_] == close) break;
                NodeT child;
                if (c == '{') {
                    usize key_start = at_;
                    std::string key;
                    if (!string(&key)) return false;
                    skip();
                    if (at_ >= s_.size() || s_[at_] != ':') return false;
                    ++at_;
                    if (!value(child, depth + 1)) return false;
                    child.key_start = key_start;
                    n.keys.push_back(std::move(key));
                } else if (!value(child, depth + 1)) {
                    return false;
                }
                n.children.push_back(std::move(child));
                skip();
                if (at_ < s_.size() && s_[at_] == ',') ++at_;
            }
            n.close = at_++;
            n.end = at_;
            return true;
        }
        if (c == '"') {
            if (!string(nullptr)) return false;
        } else {
            while (at_ < s_.size() && !is_end(s_[at_])) ++at_;
        }
        n.end = at_;
        return true;
    }
    usize at() const { return at_; }
    void skip() {
        for (;;) {
            while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
            if (s_.substr(at_).starts_with("//")) {
                while (at_ < s_.size() && s_[at_] != '\n') ++at_;
            } else if (s_.substr(at_).starts_with("/*")) {
                usize end = s_.find("*/", at_ + 2);
                at_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else {
                return;
            }
        }
    }

private:
    static bool is_end(char c) {
        return c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
    }
    bool string(std::string* out) {
        ++at_; // opening quote
        usize begin = at_;
        while (at_ < s_.size() && s_[at_] != '"') at_ += s_[at_] == '\\' ? 2 : 1;
        if (at_ >= s_.size()) return false;
        if (out) {
            // Keys are compared against parsed keys, so decode them the same way.
            auto parsed = parse(s_.substr(begin - 1, at_ - begin + 2));
            *out = std::string(parsed.value.as_string());
        }
        ++at_;
        return true;
    }

    std::string_view s_;
    usize at_ = 0;
};

} // namespace

bool TextDocument::load(std::string text) {
    text_ = std::move(text);
    auto parsed = parse(text_);
    if (!parsed) {
        error_ = parsed.error;
        return false;
    }
    error_.reset();
    value_ = std::move(parsed.value);
    return scan();
}

bool TextDocument::scan() {
    root_ = {};
    Scanner s(text_);
    if (!s.value(root_, 0)) {
        error_ = ParseError{"couldn't follow the document's layout", 0, 0};
        return false;
    }
    return true;
}

const TextDocument::Node* TextDocument::find(const Path& path) const {
    const Node* n = &root_;
    for (const Step& step : path) {
        if (const std::string* key = std::get_if<std::string>(&step)) {
            if (n->kind != '{') return nullptr;
            usize i = 0;
            while (i < n->keys.size() && n->keys[i] != *key) ++i;
            if (i == n->keys.size()) return nullptr;
            n = &n->children[i];
        } else {
            usize i = std::get<usize>(step);
            if (n->kind != '[' || i >= n->children.size()) return nullptr;
            n = &n->children[i];
        }
    }
    return n;
}

std::string TextDocument::indent_at(usize at) const {
    usize line = text_.rfind('\n', at == 0 ? 0 : at - 1);
    line = line == std::string::npos ? 0 : line + 1;
    usize end = line;
    while (end < text_.size() && (text_[end] == ' ' || text_[end] == '\t')) ++end;
    return text_.substr(line, end - line);
}

std::string TextDocument::format(const Value& value, usize at) const {
    if (!value.is_array() && !value.is_object()) return stringify(value, -1);
    std::string pretty = stringify(value, 2), indent = indent_at(at), out;
    for (char c : pretty) {
        out += c;
        if (c == '\n') out += indent;
    }
    return out;
}

bool TextDocument::replace(usize start, usize end, std::string_view with) {
    std::string next = text_.substr(0, start);
    next += with;
    next += text_.substr(end);
    return load(std::move(next));
}

bool TextDocument::set(const Path& path, const Value& value) {
    if (!valid()) return false;
    const Node* n = find(path);
    if (!n) return false;
    return replace(n->start, n->end, format(value, n->start));
}

bool TextDocument::insert(const Path& container, const Value& value, std::string_view key) {
    if (!valid()) return false;
    const Node* n = find(container);
    if (!n || !n->kind) return false;
    if (n->kind == '{') {
        if (key.empty()) return false;
        Path existing = container;
        existing.emplace_back(std::string(key));
        if (find(existing)) return set(existing, value);
    }
    std::string entry = n->kind == '{' ? stringify(Value(key), -1) + ": " : std::string();
    if (n->children.empty()) {
        // Into an empty container: on its own line when the brackets are, else inline.
        bool multiline = text_.find('\n', n->open) < n->close;
        std::string indent = indent_at(n->open);
        entry += format(value, n->open);
        std::string with = multiline ? "\n" + indent + "  " + entry + "\n" + indent : entry;
        return replace(n->open + 1, n->close, with);
    }
    const Node& last = n->children.back();
    // Lined up with the last item, on a new line if items are one per line.
    bool own_lines = text_.find('\n', n->open) < last.key_start;
    std::string indent = indent_at(last.key_start);
    entry += format(value, last.key_start);
    return replace(last.end, last.end, own_lines ? ",\n" + indent + entry : ", " + entry);
}

bool TextDocument::erase(const Path& path) {
    if (!valid() || path.empty()) return false;
    Path parent_path(path.begin(), path.end() - 1);
    const Node* parent = find(parent_path);
    const Node* n = find(path);
    if (!parent || !n) return false;
    usize index = static_cast<usize>(n - parent->children.data());
    usize start = n->key_start, end = n->end;
    if (index + 1 < parent->children.size()) {
        usize next = parent->children[index + 1].key_start;
        usize line_end = text_.find('\n', n->end);
        if (line_end < next) {
            // Items on their own lines: the item's whole line goes (with a comment at its end), and nothing else, so
            // comments above the next item stay.
            usize line = text_.rfind('\n', start == 0 ? 0 : start - 1);
            start = line == std::string::npos ? 0 : line;
            end = line_end;
        } else {
            end = next; // inline: up to the next item, taking the comma and the space
        }
    } else if (index > 0) {
        start = parent->children[index - 1].end; // from the comma after the previous item
        // A trailing comma after this last item goes too.
        usize after = end;
        while (after < text_.size() && (text_[after] == ' ' || text_[after] == '\t')) ++after;
        if (after < text_.size() && text_[after] == ',') end = after + 1;
        // And a comment at the end of its line.
        after = end;
        while (after < text_.size() && (text_[after] == ' ' || text_[after] == '\t')) ++after;
        if (text_.compare(after, 2, "//") == 0) end = std::min(text_.find('\n', after), text_.size());
    } else {
        // The only item: leave the brackets as they were written, empty.
        start = parent->open + 1;
        end = parent->close;
        return replace(start, end, "");
    }
    return replace(start, end, "");
}

} // namespace bowl::json
