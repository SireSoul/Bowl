#include "ph/core/xml.hpp"

#include "ph/core/utf8.hpp"

#include <charconv>
#include <format>

namespace ph::xml {

std::string_view Element::attribute(std::string_view key, std::string_view fallback) const {
    for (const auto& [k, v] : attributes)
        if (k == key) return v;
    return fallback;
}

bool Element::has(std::string_view key) const {
    for (const auto& a : attributes)
        if (a.first == key) return true;
    return false;
}

const Element* Element::child(std::string_view child_name) const {
    for (const Element& c : children)
        if (c.name == child_name) return &c;
    return nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    ParseResult run() {
        ParseResult out;
        if (s_.starts_with("\xEF\xBB\xBF")) at_ = 3;
        if (!skip_misc() || !expect('<')) return fail(out, "no root element");
        --at_;
        if (!element(out.root, 0)) return fail(out, error_);
        if (!skip_misc() || at_ != s_.size()) return fail(out, error_.empty() ? "text after the root element" : error_);
        return out;
    }

private:
    ParseResult& fail(ParseResult& out, std::string_view message) {
        u32 line = 1, column = 1;
        for (usize i = 0; i < at_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        out.error = ParseError{std::string(message), line, column};
        return out;
    }

    bool error(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }

    bool expect(char c) {
        if (at_ < s_.size() && s_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    void skip_space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
    }

    // Skips to just past `end`.
    bool skip_past(std::string_view end) {
        usize found = s_.find(end, at_);
        if (found == std::string_view::npos) return error(std::format("missing '{}'", end));
        at_ = found + end.size();
        return true;
    }

    // Whitespace, comments, <?...?> and <!DOCTYPE ...> outside the root element.
    bool skip_misc() {
        for (;;) {
            skip_space();
            if (s_.substr(at_).starts_with("<?")) {
                if (!skip_past("?>")) return false;
            } else if (s_.substr(at_).starts_with("<!--")) {
                if (!skip_past("-->")) return false;
            } else if (s_.substr(at_).starts_with("<!")) {
                if (!skip_past(">")) return false;
            } else {
                return true;
            }
        }
    }

    static bool name_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == ':' || static_cast<u8>(c) >= 0x80;
    }

    std::string_view name() {
        usize start = at_;
        while (at_ < s_.size() && name_char(s_[at_])) ++at_;
        return s_.substr(start, at_ - start);
    }

    // Appends `raw` with entities decoded.
    bool decode(std::string_view raw, std::string& out) {
        for (usize i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            usize semi = raw.find(';', i);
            if (semi == std::string_view::npos) return error("'&' without ';'");
            std::string_view entity = raw.substr(i + 1, semi - i - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (entity.starts_with('#')) {
                bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                std::string_view digits = entity.substr(hex ? 2 : 1);
                u32 cp = 0;
                if (std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10).ec != std::errc{})
                    return error(std::format("bad character reference '&{};'", entity));
                utf8::append(out, cp);
            } else {
                return error(std::format("unknown entity '&{};'", entity));
            }
            i = semi;
        }
        return true;
    }

    bool element(Element& e, int depth) {
        if (depth > 256) return error("elements nested too deeply");
        expect('<');
        e.name = std::string(name());
        if (e.name.empty()) return error("expected an element name");
        for (;;) { // attributes
            skip_space();
            if (expect('/')) {
                if (!expect('>')) return error("expected '>' after '/'");
                return true;
            }
            if (expect('>')) break;
            std::string key(name());
            if (key.empty()) return error(std::format("unexpected character in <{}>", e.name));
            skip_space();
            if (!expect('=')) return error(std::format("attribute '{}' has no value", key));
            skip_space();
            char quote = at_ < s_.size() ? s_[at_] : 0;
            if (quote != '"' && quote != '\'') return error(std::format("attribute '{}' isn't quoted", key));
            ++at_;
            usize end = s_.find(quote, at_);
            if (end == std::string_view::npos) return error(std::format("attribute '{}' isn't closed", key));
            std::string value;
            if (!decode(s_.substr(at_, end - at_), value)) return false;
            at_ = end + 1;
            e.attributes.emplace_back(std::move(key), std::move(value));
        }
        for (;;) { // content
            usize lt = s_.find('<', at_);
            if (lt == std::string_view::npos) return error(std::format("<{}> isn't closed", e.name));
            if (!decode(s_.substr(at_, lt - at_), e.text)) return false;
            at_ = lt;
            std::string_view rest = s_.substr(at_);
            if (rest.starts_with("</")) {
                at_ += 2;
                std::string_view closing = name();
                skip_space();
                if (closing != e.name || !expect('>'))
                    return error(std::format("<{}> closed by </{}>", e.name, closing));
                return true;
            }
            if (rest.starts_with("<!--")) {
                if (!skip_past("-->")) return false;
            } else if (rest.starts_with("<![CDATA[")) {
                usize end = s_.find("]]>", at_);
                if (end == std::string_view::npos) return error("CDATA isn't closed");
                e.text += s_.substr(at_ + 9, end - at_ - 9);
                at_ = end + 3;
            } else if (rest.starts_with("<?")) {
                if (!skip_past("?>")) return false;
            } else {
                e.children.emplace_back();
                if (!element(e.children.back(), depth + 1)) return false;
            }
        }
    }

    std::string_view s_;
    usize at_ = 0;
    std::string error_;
};

} // namespace

ParseResult parse(std::string_view text) { return Parser(text).run(); }

} // namespace ph::xml
