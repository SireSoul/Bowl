#include "bowl/apidoc/apidoc.hpp"

#include <algorithm>
#include <cctype>

namespace bowl::apidoc {

namespace {

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool starts_with_word(std::string_view s, std::string_view word) {
    return s.starts_with(word) && (s.size() == word.size() || !is_ident(s[word.size()]));
}

// "template <class T> struct X" -> "struct X": what follows the template parameter lists.
std::string_view after_templates(std::string_view s) {
    while (starts_with_word(s, "template")) {
        usize open = s.find('<');
        if (open == std::string_view::npos) return s;
        int depth = 0;
        usize i = open;
        for (; i < s.size(); ++i) {
            if (s[i] == '<') ++depth;
            if (s[i] == '>' && --depth == 0) break;
        }
        s = trim(s.substr(std::min(i + 1, s.size())));
    }
    return s;
}

// The identifier after a keyword, skipping attributes: "struct [[nodiscard]] Foo : Bar" -> "Foo".
std::string name_after(std::string_view s, std::string_view keyword) {
    s = trim(s.substr(keyword.size()));
    while (s.starts_with("[[")) {
        usize end = s.find("]]");
        if (end == std::string_view::npos) return {};
        s = trim(s.substr(end + 2));
    }
    if (starts_with_word(s, "class")) s = trim(s.substr(5)); // enum class
    usize n = 0;
    while (n < s.size() && is_ident(s[n])) ++n;
    return std::string(s.substr(0, n));
}

// A definition of something declared elsewhere ("Asset<T> AssetManager::load(...)"): the name before the first
// parenthesis is qualified.
bool is_out_of_class_definition(std::string_view code) {
    usize paren = code.find('(');
    if (paren == std::string_view::npos) return false;
    usize end = paren;
    while (end > 0 && code[end - 1] == ' ') --end;
    usize begin = end;
    while (begin > 0 && (is_ident(code[begin - 1]) || code[begin - 1] == ':' || code[begin - 1] == '~')) --begin;
    std::string_view name = code.substr(begin, end - begin);
    if (name.starts_with("std::")) return false;
    return name.find("::") != std::string_view::npos;
}

// "Foo(int a) : a_(a)" -> "Foo(int a)": a constructor's member initializers aren't part of its interface.
std::string without_initializers(std::string_view code) {
    usize open = code.find('(');
    if (open == std::string_view::npos) return std::string(code);
    int depth = 0;
    usize close = open;
    for (; close < code.size(); ++close) {
        if (code[close] == '(') ++depth;
        if (code[close] == ')' && --depth == 0) break;
    }
    for (usize i = close + 1; i < code.size(); ++i)
        if (code[i] == ':' && (i + 1 >= code.size() || code[i + 1] != ':') && code[i - 1] != ':')
            return std::string(trim(code.substr(0, i)));
    return std::string(code);
}

// Removes the source's indentation from the lines after the first (the first starts where the statement did).
std::string dedent(std::string_view text, usize column) {
    std::string out;
    usize at = 0;
    bool first = true;
    while (at <= text.size()) {
        usize end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
        if (!first) {
            usize spaces = 0;
            while (spaces < line.size() && spaces < column && line[spaces] == ' ') ++spaces;
            line.remove_prefix(spaces);
            out += '\n';
        }
        out += line;
        first = false;
        at = end + 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

class Parser {
public:
    explicit Parser(std::string_view source) : s_(source) {}

    Header run(std::string_view path) {
        Header h;
        h.path = std::string(path);
        header_ = &h;
        scope(false, true, &h.intro, h.items);
        return h;
    }

private:
    struct Statement {
        std::string text;
        char end = 0; // ';', '{', ')' (a macro call on its own line) or '}' (the scope ended)
        usize column = 0; // where it started on its line
        std::string trailing;
    };

    char peek(usize k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    bool at(std::string_view t) const { return s_.substr(i_, t.size()) == t; }

    std::string_view rest_of_line() {
        usize end = s_.find('\n', i_);
        if (end == std::string_view::npos) end = s_.size();
        std::string_view line = s_.substr(i_, end - i_);
        i_ = end;
        return line;
    }

    usize column(usize pos) const {
        usize line_start = s_.rfind('\n', pos == 0 ? 0 : pos - 1);
        return line_start == std::string_view::npos ? pos : pos - line_start - 1;
    }

    void skip_literal() {
        char quote = s_[i_++];
        while (i_ < s_.size() && s_[i_] != quote) {
            if (s_[i_] == '\\') ++i_;
            ++i_;
        }
        ++i_;
    }

    // From just after a '{' to just after its '}'.
    void skip_block() {
        int depth = 1;
        while (i_ < s_.size() && depth > 0) {
            if (at("//")) {
                rest_of_line();
                continue;
            }
            if (at("/*")) {
                usize end = s_.find("*/", i_ + 2);
                i_ = end == std::string_view::npos ? s_.size() : end + 2;
                continue;
            }
            char c = s_[i_];
            if (c == '"' || c == '\'') {
                skip_literal();
                continue;
            }
            if (c == '{') ++depth;
            if (c == '}') --depth;
            ++i_;
        }
    }

    void skip_spaces() {
        while (peek() == ' ' || peek() == '\t') ++i_;
    }

    std::string trailing_comment() {
        usize from = i_;
        skip_spaces();
        last_pad_ = std::max<usize>(1, i_ - from);
        if (!at("//")) return {};
        i_ += 2;
        return std::string(trim(rest_of_line()));
    }

    Statement statement() {
        usize start = i_;
        int paren = 0;
        // A macro call (BOWL_REFLECT(...)) may end its line with no semicolon.
        usize n = 0;
        while (start + n < s_.size() && is_ident(s_[start + n])) ++n;
        std::string_view first = s_.substr(start, n);
        bool macro = n > 0 && std::none_of(first.begin(), first.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); }) &&
                     std::any_of(first.begin(), first.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
        Statement st;
        st.column = column(start);
        while (i_ < s_.size()) {
            if (at("//")) {
                rest_of_line();
                continue;
            }
            if (at("/*")) {
                usize end = s_.find("*/", i_ + 2);
                i_ = end == std::string_view::npos ? s_.size() : end + 2;
                continue;
            }
            char c = s_[i_];
            if (c == '"' || c == '\'') {
                skip_literal();
                continue;
            }
            if (c == '(') ++paren;
            if (c == ')' && --paren == 0 && macro) {
                usize after = i_ + 1;
                while (after < s_.size() && (s_[after] == ' ' || s_[after] == '\t')) ++after;
                if (after >= s_.size() || s_[after] == '\n' || s_.substr(after, 2) == "//") {
                    st.text = dedent(s_.substr(start, i_ + 1 - start), column(start));
                    st.end = ')';
                    ++i_;
                    st.trailing = trailing_comment();
                    return st;
                }
            }
            if (paren == 0 && (c == ';' || c == '{' || c == '}')) {
                st.text = dedent(trim(s_.substr(start, i_ - start)), column(start));
                st.end = c;
                if (c != '}') ++i_;
                if (c == ';') st.trailing = trailing_comment();
                return st;
            }
            ++i_;
        }
        st.text = dedent(trim(s_.substr(start)), column(start));
        st.end = '}';
        return st;
    }

    void scope(bool in_class, bool is_public, std::vector<std::string>* intro, std::vector<Item>& out) {
        std::vector<std::string> comment;
        bool line_has_content = false, gap = false, public_ = is_public;
        auto add = [&](Item item) {
            item.comment = std::move(comment);
            item.gap_before = gap;
            comment.clear();
            gap = false;
            if (public_) out.push_back(std::move(item));
        };
        while (i_ < s_.size()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i_;
                continue;
            }
            if (c == '\n') {
                ++i_;
                if (!line_has_content) { // a blank line: a comment above it stands on its own
                    if (!comment.empty()) {
                        if (intro && intro->empty() && out.empty()) {
                            *intro = std::move(comment);
                            comment.clear();
                        } else {
                            Item note;
                            note.kind = Item::Kind::Note;
                            add(std::move(note));
                        }
                    }
                    gap = true;
                }
                line_has_content = false;
                continue;
            }
            line_has_content = true;
            if (at("//")) {
                i_ += 2;
                std::string_view line = rest_of_line();
                if (line.starts_with(' ')) line.remove_prefix(1);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
                comment.emplace_back(line);
                continue;
            }
            if (at("/*")) {
                usize end = s_.find("*/", i_ + 2);
                i_ = end == std::string_view::npos ? s_.size() : end + 2;
                continue;
            }
            if (c == '#') {
                std::string_view line = rest_of_line();
                std::string first(trim(line));
                while (!line.empty() && line.back() == '\\' && i_ < s_.size()) { // continued on the next line
                    ++i_;
                    line = rest_of_line();
                }
                if (first.starts_with("#define") && !in_class) {
                    if (first.ends_with("\\")) first = std::string(trim(std::string_view(first).substr(0, first.size() - 1)));
                    Item d;
                    d.code = first;
                    add(std::move(d));
                } else {
                    comment.clear();
                }
                continue;
            }
            if (c == '}') { // the end of this scope
                ++i_;
                skip_spaces();
                if (peek() == ';') ++i_;
                trailing_comment();
                return;
            }
            if (in_class && (at("public:") || at("private:") || at("protected:"))) {
                public_ = at("public:");
                i_ = s_.find(':', i_) + 1;
                comment.clear();
                gap = false;
                continue;
            }

            Statement st = statement();
            if (st.end == '}') continue; // a stray brace; the loop ends the scope
            std::string_view code = st.text;
            std::string_view bare = after_templates(code);
            if (st.end == ')') { // a macro call
                Item d;
                d.code = st.text;
                d.trailing = st.trailing;
                d.trailing_pad = last_pad_;
                add(std::move(d));
                continue;
            }
            if (st.end == '{') {
                if (starts_with_word(code, "namespace")) {
                    std::string name(trim(code.substr(9)));
                    if (name.empty() || name.find("detail") != std::string::npos || in_class) {
                        skip_block();
                        trailing_comment(); // "} // namespace detail"
                    } else {
                        if (std::find(header_->namespaces.begin(), header_->namespaces.end(), name) == header_->namespaces.end())
                            header_->namespaces.push_back(name);
                        trailing_comment();
                        comment.clear();
                        scope(false, true, nullptr, out); // flattened into this one
                    }
                    comment.clear();
                    gap = false;
                    continue;
                }
                if (starts_with_word(code, "extern")) {
                    scope(false, true, nullptr, out);
                    continue;
                }
                bool is_enum = starts_with_word(bare, "enum");
                std::string keyword = starts_with_word(bare, "class")    ? "class"
                                      : starts_with_word(bare, "struct") ? "struct"
                                      : starts_with_word(bare, "union")  ? "union"
                                                                         : "";
                if (!keyword.empty() && bare.find('(') == std::string_view::npos) {
                    Item cls;
                    cls.kind = Item::Kind::Class;
                    cls.code = st.text;
                    cls.name = name_after(bare, keyword);
                    cls.trailing = trailing_comment();
                    std::vector<std::string> above = std::move(comment);
                    comment.clear();
                    scope(true, keyword != "class", nullptr, cls.members);
                    comment = std::move(above);
                    add(std::move(cls));
                    continue;
                }
                usize body = i_;
                skip_block();
                std::string braces = "{" + std::string(s_.substr(body, i_ - body));
                skip_spaces();
                if (peek() == ';') ++i_;
                std::string trailing = trailing_comment();
                if (is_enum) { // the values are the point
                    Item d;
                    d.code = dedent(st.text + " " + braces + ";", st.column);
                    d.trailing = trailing;
                    d.trailing_pad = last_pad_;
                    add(std::move(d));
                    continue;
                }
                // A function with its body (dropped), or a variable with braces (kept: they're its value).
                usize paren = code.find('('), equals = code.find('=');
                bool variable = paren == std::string_view::npos || (equals != std::string_view::npos && equals < paren);
                if (in_class || !is_out_of_class_definition(code)) {
                    Item d;
                    d.code = variable ? st.text + braces + ";" : without_initializers(st.text) + ";";
                    d.trailing = trailing;
                    d.trailing_pad = last_pad_;
                    add(std::move(d));
                } else {
                    comment.clear();
                    gap = false;
                }
                continue;
            }
            // Ended by ';'.
            bool forward = (starts_with_word(bare, "class") || starts_with_word(bare, "struct")) &&
                           bare.find_first_of("{:(") == std::string_view::npos && bare.find(' ', 7) == std::string_view::npos;
            if (forward || starts_with_word(code, "friend") || starts_with_word(code, "static_assert") ||
                code.starts_with("using namespace") || (!in_class && is_out_of_class_definition(code))) {
                comment.clear();
                gap = false;
                continue;
            }
            Item d;
            d.code = st.text + ";";
            d.trailing = st.trailing;
                d.trailing_pad = last_pad_;
            add(std::move(d));
        }
    }

    std::string_view s_;
    usize i_ = 0;
    Header* header_ = nullptr;
    usize last_pad_ = 1; // spaces before the last trailing comment
};

// Markdown

// '<' outside backticks would start an HTML tag on GitHub.
std::string escape_prose(std::string_view line) {
    std::string out;
    bool code = false;
    for (char c : line) {
        if (c == '`') code = !code;
        if (!code && c == '<') out += "&lt;";
        else out += c;
    }
    return out;
}

// Comment lines as Markdown: lines indented four spaces are code examples.
std::string render_comment(const std::vector<std::string>& lines) {
    std::string out;
    bool in_code = false;
    for (const std::string& line : lines) {
        bool code_line = line.starts_with("    ") || (in_code && trim(line).empty());
        if (code_line && !in_code) {
            out += "```cpp\n";
            in_code = true;
        } else if (!code_line && in_code) {
            while (out.ends_with("\n\n")) out.pop_back();
            out += "```\n";
            in_code = false;
        }
        out += in_code ? (line.size() >= 4 ? line.substr(4) : std::string()) : escape_prose(line);
        out += '\n';
    }
    if (in_code) {
        while (out.ends_with("\n\n")) out.pop_back();
        out += "```\n";
    }
    return out;
}

std::string with_trailing(const Item& item) {
    return item.trailing.empty() ? item.code : item.code + std::string(item.trailing_pad, ' ') + "// " + item.trailing;
}

// A struct of plain fields reads best whole.
bool data_only(const Item& cls) {
    for (const Item& m : cls.members) {
        if (m.kind == Item::Kind::Class) return false;
        if (m.kind != Item::Kind::Decl) continue;
        usize paren = m.code.find('('), equals = m.code.find('=');
        if (paren != std::string::npos && (equals == std::string::npos || equals > paren)) return false;
    }
    return true;
}

std::string indent(std::string_view text, std::string_view by) {
    std::string out;
    usize at = 0;
    while (at <= text.size()) {
        usize end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        if (!line.empty()) out += by;
        out += line;
        out += '\n';
        at = end + 1;
    }
    return out;
}

void render_items(std::string& out, const std::vector<Item>& items, int level);

void render_class(std::string& out, const Item& cls, int level) {
    out += std::string(static_cast<usize>(level), '#') + " " + cls.name + "\n\n";
    if (!cls.comment.empty()) out += render_comment(cls.comment) + "\n";
    if (data_only(cls)) {
        out += "```cpp\n" + cls.code + " {" + (cls.trailing.empty() ? "" : " // " + cls.trailing) + "\n";
        for (const Item& m : cls.members) {
            for (const std::string& line : m.comment) out += "    //" + (line.empty() ? "" : " " + line) + "\n";
            if (m.kind == Item::Kind::Decl) out += indent(with_trailing(m), "    ");
        }
        out += "};\n```\n\n";
        return;
    }
    out += "```cpp\n" + cls.code + "\n```\n\n";
    render_items(out, cls.members, std::min(level + 1, 6));
}

void render_items(std::string& out, const std::vector<Item>& items, int level) {
    for (usize i = 0; i < items.size();) {
        const Item& item = items[i];
        if (item.kind == Item::Kind::Note) {
            // A short line on its own ("Entities", "Raw devices") labels the declarations below it.
            const auto& c = item.comment;
            bool label = c.size() == 1 && !c[0].empty() && c[0].size() <= 40 && c[0].find_first_of(".:;`") == std::string::npos;
            out += label ? "**" + escape_prose(c[0]) + "**\n\n" : render_comment(c) + "\n";
            ++i;
            continue;
        }
        if (item.kind == Item::Kind::Class) {
            render_class(out, item, level);
            ++i;
            continue;
        }
        // A declaration, and the undocumented ones right below it.
        usize j = i + 1;
        while (j < items.size() && items[j].kind == Item::Kind::Decl && items[j].comment.empty() && !items[j].gap_before) ++j;
        if (!item.comment.empty()) out += render_comment(item.comment) + "\n";
        out += "```cpp\n";
        for (usize k = i; k < j; ++k) out += with_trailing(items[k]) + "\n";
        out += "```\n\n";
        i = j;
    }
}

std::string first_sentence(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        if (line.starts_with("    ") || trim(line).empty()) break;
        if (!text.empty()) text += ' ';
        text += trim(line);
    }
    // The first ". " that ends a sentence: not inside parentheses or backticks, not "e.g." or "...".
    int depth = 0;
    bool code = false;
    for (usize i = 0; i + 1 < text.size(); ++i) {
        char c = text[i];
        if (c == '`') code = !code;
        if (code) continue;
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c != '.' || text[i + 1] != ' ' || depth > 0) continue;
        std::string_view before = std::string_view(text).substr(0, i);
        if (before.ends_with(".") || before.ends_with("e.g") || before.ends_with("i.e") || before.ends_with("etc")) continue;
        text.resize(i + 1);
        break;
    }
    if (text.ends_with(':')) text.back() = '.';
    return escape_prose(text);
}

constexpr std::string_view generated_note =
    "Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run\n"
    "`cmake --build <build folder> --target bowl_api_docs`.\n";

} // namespace

Header parse(std::string_view path, std::string_view source) { return Parser(source).run(path); }

std::string anchor(std::string_view heading) {
    std::string out;
    for (char c : heading) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (c == ' ') out += '-';
    }
    return out;
}

std::string render_module(std::string_view module, const std::vector<Header>& headers) {
    std::string out = "# " + std::string(module) + "\n\n" + std::string(generated_note) + "\n";
    for (const Header& h : headers) out += "- [" + h.path + "](#" + anchor(h.path) + ")\n";
    out += "\n";
    for (const Header& h : headers) {
        out += "## " + h.path + "\n\n";
        out += "`#include \"" + h.path + "\"`";
        if (!h.namespaces.empty()) {
            out += " · namespace ";
            for (usize i = 0; i < h.namespaces.size(); ++i) out += (i ? ", `" : "`") + h.namespaces[i] + "`";
        }
        out += "\n\n";
        if (!h.intro.empty()) out += render_comment(h.intro) + "\n";
        render_items(out, h.items, 3);
    }
    while (out.ends_with("\n\n")) out.pop_back();
    return out;
}

std::string render_index(const std::vector<std::pair<std::string, std::vector<Header>>>& modules) {
    std::string out = "# API reference\n\n" + std::string(generated_note) +
                      "\nEvery public header, by module. The [guides](../../README.md#guides) explain how the pieces "
                      "fit together.\n\n| Header | |\n| --- | --- |\n";
    for (const auto& [module, headers] : modules)
        for (const Header& h : headers) {
            std::string about = first_sentence(h.intro);
            if (about.empty()) // no introduction: the first documented thing in it
                for (const Item& item : h.items)
                    if (!item.comment.empty()) {
                        about = first_sentence(item.comment);
                        break;
                    }
            for (char& c : about)
                if (c == '|') c = '/';
            out += "| [" + h.path + "](" + module + ".md#" + anchor(h.path) + ") | " + about + " |\n";
        }
    return out;
}

} // namespace bowl::apidoc
