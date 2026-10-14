#include "ph/core/json.hpp"

#include "ph/core/utf8.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace ph::json {

namespace {
const Value null_value;
const Array empty_array;
const Object empty_object;
} // namespace

usize Value::size() const {
    if (is_array()) return std::get<Array>(data_).size();
    if (is_object()) return std::get<Object>(data_).size();
    return 0;
}

const Value& Value::operator[](usize index) const {
    if (!is_array()) return null_value;
    const Array& a = std::get<Array>(data_);
    return index < a.size() ? a[index] : null_value;
}

const Value* Value::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : std::get<Object>(data_))
        if (k == key) return &v;
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : null_value;
}

const Array& Value::array() const { return is_array() ? std::get<Array>(data_) : empty_array; }
const Object& Value::object() const { return is_object() ? std::get<Object>(data_) : empty_object; }

Array& Value::make_array() {
    if (!is_array()) data_ = Array{};
    return std::get<Array>(data_);
}

Object& Value::make_object() {
    if (!is_object()) data_ = Object{};
    return std::get<Object>(data_);
}

Value& Value::set(std::string_view key, Value value) {
    Object& o = make_object();
    for (auto& [k, v] : o)
        if (k == key) return v = std::move(value);
    o.emplace_back(std::string(key), std::move(value));
    return o.back().second;
}

// Parser

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    ParseResult run() {
        ParseResult r;
        skip();
        if (!parse_value(r.value, 0)) {
            r.error = error_;
            r.value = {};
            return r;
        }
        skip();
        if (!failed_ && pos_ != s_.size()) fail("unexpected text after the document");
        if (failed_) {
            r.error = error_;
            r.value = {};
        }
        return r;
    }

private:
    bool fail(std::string message) {
        if (failed_) return false;
        failed_ = true;
        u32 line = 1, col = 1;
        for (usize i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        error_ = {std::move(message), line, col};
        return false;
    }

    void skip() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                usize end = s_.find("*/", pos_ + 2);
                pos_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > 256) return fail("nested too deeply");
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        char c = s_[pos_];
        if (c == '{') return parse_object(out, depth);
        if (c == '[') return parse_array(out, depth);
        if (c == '"') {
            std::string str;
            if (!parse_string(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        if (literal("true")) { out = true; return true; }
        if (literal("false")) { out = false; return true; }
        if (literal("null")) { out = nullptr; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return fail(std::format("unexpected character '{}'", c));
    }

    bool parse_number(Value& out) {
        usize start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        f64 value = 0;
        auto [end, ec] = std::from_chars(s_.data() + start, s_.data() + pos_, value);
        if (ec != std::errc() || end != s_.data() + pos_) {
            pos_ = start;
            return fail("malformed number");
        }
        out = value;
        return true;
    }

    bool hex4(u32& out) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char h = s_[pos_++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= static_cast<u32>(h - '0');
            else if (h >= 'a' && h <= 'f') out |= static_cast<u32>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') out |= static_cast<u32>(h - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                u32 cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) { // surrogate pair
                    u32 low = 0;
                    if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return fail("unpaired surrogate in \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8::append(out, cp);
                break;
            }
            default: return fail(std::format("unknown escape '\\{}'", e));
            }
        }
        return fail("unterminated string");
    }

    bool parse_array(Value& out, int depth) {
        ++pos_;
        Array& a = out.make_array();
        skip();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            a.push_back(std::move(v));
            skip();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ']') { ++pos_; return true; }
            if (s_[pos_] != ',') return fail("expected ',' or ']'");
            ++pos_;
            skip();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; } // trailing comma
        }
    }

    bool parse_object(Value& out, int depth) {
        ++pos_;
        Object& o = out.make_object();
        skip();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a key string");
            std::string key;
            if (!parse_string(key)) return false;
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':' after key");
            ++pos_;
            skip();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            o.emplace_back(std::move(key), std::move(v));
            skip();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == '}') { ++pos_; return true; }
            if (s_[pos_] != ',') return fail("expected ',' or '}'");
            ++pos_;
            skip();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; } // trailing comma
        }
    }

    std::string_view s_;
    usize pos_ = 0;
    bool failed_ = false;
    ParseError error_;
};

void write_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<int>(c));
            else out += c;
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, int indent, int level) {
    auto newline = [&](int lvl) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<usize>(indent * lvl), ' ');
    };
    if (v.is_null()) out += "null";
    else if (v.is_bool()) out += v.as_bool() ? "true" : "false";
    else if (v.is_number()) {
        f64 n = v.as_number();
        if (!std::isfinite(n)) out += "null";
        else if (n == std::floor(n) && std::abs(n) < 1e15) out += std::format("{}", static_cast<i64>(n));
        else out += std::format("{}", n);
    } else if (v.is_string()) write_string(out, v.as_string());
    else if (v.is_array()) {
        if (v.size() == 0) { out += "[]"; return; }
        out += '[';
        bool first = true;
        for (const Value& item : v.array()) {
            if (!first) out += ',';
            first = false;
            newline(level + 1);
            write(out, item, indent, level + 1);
        }
        newline(level);
        out += ']';
    } else {
        if (v.size() == 0) { out += "{}"; return; }
        out += '{';
        bool first = true;
        for (const auto& [key, item] : v.object()) {
            if (!first) out += ',';
            first = false;
            newline(level + 1);
            write_string(out, key);
            out += indent < 0 ? ":" : ": ";
            write(out, item, indent, level + 1);
        }
        newline(level);
        out += '}';
    }
}

} // namespace

ParseResult parse(std::string_view text) { return Parser(text).run(); }

std::string stringify(const Value& value, int indent) {
    std::string out;
    write(out, value, indent, 0);
    return out;
}

} // namespace ph::json
