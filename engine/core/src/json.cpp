#include "bowl/core/json.hpp"

#include "bowl/core/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <unordered_map>

namespace bowl::json {

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

Value* Value::find(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

bool Value::erase(std::string_view key) {
    if (!is_object()) return false;
    Object& o = std::get<Object>(data_);
    auto it = std::find_if(o.begin(), o.end(), [&](const auto& kv) { return kv.first == key; });
    if (it == o.end()) return false;
    o.erase(it);
    return true;
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
#if defined(__APPLE__)
        // Apple's floating-point from_chars needs iOS 26 / macOS 26; strtod reads the same text ("C" locale, which
        // games run in: nothing calls setlocale).
        std::string number(s_.substr(start, pos_ - start));
        char* end = nullptr;
        value = std::strtod(number.c_str(), &end);
        bool ok = !number.empty() && end == number.c_str() + number.size();
#else
        auto [end, ec] = std::from_chars(s_.data() + start, s_.data() + pos_, value);
        bool ok = ec == std::errc() && end == s_.data() + pos_;
#endif
        if (!ok) {
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

// Binary form
//
//   "BWJB" u8 version
//   varint string count, then each string: varint length, bytes
//   the value: a tag byte, then
//     null, false, true                nothing
//     integer                          zigzag varint (whole numbers up to 2^53)
//     f32, f64                         4 or 8 bytes, little-endian (f32 when it holds the number exactly)
//     string                           varint index into the strings
//     array                            varint count, values
//     object                           varint count, then (varint key index, value) pairs

namespace {

enum Tag : u8 { Null, False, True, Integer, Float32, Float64, String, ArrayTag, ObjectTag };
constexpr u8 binary_version = 1;

class BinaryWriter {
public:
    std::vector<u8> run(const Value& root) {
        collect(root);
        out_ = {'B', 'W', 'J', 'B', binary_version};
        varint(strings_.size());
        for (const std::string* s : strings_) {
            varint(s->size());
            out_.insert(out_.end(), s->begin(), s->end());
        }
        value(root);
        return std::move(out_);
    }

private:
    void intern(const std::string& s) {
        if (index_.emplace(s, strings_.size()).second) strings_.push_back(&index_.find(s)->first);
    }
    void collect(const Value& v) {
        if (v.is_string()) intern(std::string(v.as_string()));
        else if (v.is_array())
            for (const Value& item : v.array()) collect(item);
        else if (v.is_object())
            for (const auto& [k, item] : v.object()) {
                intern(k);
                collect(item);
            }
    }
    void varint(u64 n) {
        while (n >= 0x80) {
            out_.push_back(static_cast<u8>(n | 0x80));
            n >>= 7;
        }
        out_.push_back(static_cast<u8>(n));
    }
    template <class T> void raw(T x) {
        u8 bytes[sizeof(T)];
        std::memcpy(bytes, &x, sizeof(T));
        out_.insert(out_.end(), bytes, bytes + sizeof(T)); // little-endian hosts only, like every target platform
    }
    void value(const Value& v) {
        if (v.is_null()) out_.push_back(Null);
        else if (v.is_bool()) out_.push_back(v.as_bool() ? True : False);
        else if (v.is_number()) {
            f64 x = v.as_number();
            if (std::trunc(x) == x && std::abs(x) <= 9007199254740992.0 && !(x == 0 && std::signbit(x))) {
                out_.push_back(Integer);
                i64 i = static_cast<i64>(x);
                varint((static_cast<u64>(i) << 1) ^ static_cast<u64>(i >> 63));
            } else if (static_cast<f64>(static_cast<f32>(x)) == x) {
                out_.push_back(Float32);
                raw(static_cast<f32>(x));
            } else {
                out_.push_back(Float64);
                raw(x);
            }
        } else if (v.is_string()) {
            out_.push_back(String);
            varint(index_.at(std::string(v.as_string())));
        } else if (v.is_array()) {
            out_.push_back(ArrayTag);
            varint(v.size());
            for (const Value& item : v.array()) value(item);
        } else {
            out_.push_back(ObjectTag);
            varint(v.size());
            for (const auto& [k, item] : v.object()) {
                varint(index_.at(k));
                value(item);
            }
        }
    }

    std::vector<u8> out_;
    std::unordered_map<std::string, u64> index_;
    std::vector<const std::string*> strings_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const u8> data) : d_(data) {}

    ParseResult run() {
        ParseResult out;
        if (!is_binary(d_)) return fail(out, "not binary JSON (no \"BWJB\")");
        if (d_[4] > binary_version) return fail(out, "written by a newer version of the format");
        at_ = 5;
        u64 count = 0;
        if (!varint(count) || count > d_.size()) return fail(out, "bad string table");
        strings_.reserve(count);
        for (u64 i = 0; i < count; ++i) {
            u64 length = 0;
            if (!varint(length) || length > d_.size() - at_) return fail(out, "bad string table");
            strings_.emplace_back(reinterpret_cast<const char*>(d_.data() + at_), length);
            at_ += length;
        }
        if (!value(out.value, 0)) return fail(out, error_);
        if (at_ != d_.size()) return fail(out, "bytes after the document");
        return out;
    }

private:
    ParseResult& fail(ParseResult& out, std::string_view message) {
        out.value = {};
        out.error = ParseError{std::string(message), 0, static_cast<u32>(at_)};
        return out;
    }
    bool error(std::string_view message) {
        if (error_.empty()) error_ = message;
        return false;
    }
    bool varint(u64& n) {
        n = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (at_ >= d_.size()) return false;
            u8 b = d_[at_++];
            n |= static_cast<u64>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool string_ref(std::string& s) {
        u64 i = 0;
        if (!varint(i) || i >= strings_.size()) return error("bad string index");
        s = strings_[i];
        return true;
    }
    template <class T> bool raw(T& x) {
        if (d_.size() - at_ < sizeof(T)) return error("truncated number");
        std::memcpy(&x, d_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 512) return error("nested too deeply");
        if (at_ >= d_.size()) return error("truncated");
        switch (d_[at_++]) {
        case Null: v = nullptr; return true;
        case False: v = false; return true;
        case True: v = true; return true;
        case Integer: {
            u64 z = 0;
            if (!varint(z)) return error("bad integer");
            v = static_cast<f64>(static_cast<i64>((z >> 1) ^ (~(z & 1) + 1)));
            return true;
        }
        case Float32: {
            f32 x = 0;
            if (!raw(x)) return false;
            v = static_cast<f64>(x);
            return true;
        }
        case Float64: {
            f64 x = 0;
            if (!raw(x)) return false;
            v = x;
            return true;
        }
        case String: {
            std::string s;
            if (!string_ref(s)) return false;
            v = std::move(s);
            return true;
        }
        case ArrayTag: {
            u64 n = 0;
            if (!varint(n) || n > d_.size() - at_) return error("bad array");
            Array& a = v.make_array();
            a.resize(n);
            for (Value& item : a)
                if (!value(item, depth + 1)) return false;
            return true;
        }
        case ObjectTag: {
            u64 n = 0;
            if (!varint(n) || n > d_.size() - at_) return error("bad object");
            Object& o = v.make_object();
            o.resize(n);
            for (auto& [k, item] : o)
                if (!string_ref(k) || !value(item, depth + 1)) return false;
            return true;
        }
        default: return error("unknown value tag");
        }
    }

    std::span<const u8> d_;
    usize at_ = 0;
    std::vector<std::string> strings_;
    std::string error_;
};

} // namespace

std::vector<u8> encode_binary(const Value& value) { return BinaryWriter().run(value); }

bool is_binary(std::span<const u8> data) { return data.size() >= 5 && std::memcmp(data.data(), "BWJB", 4) == 0; }

ParseResult decode_binary(std::span<const u8> data) { return BinaryReader(data).run(); }

ParseResult parse_any(std::span<const u8> data) {
    if (is_binary(data)) return decode_binary(data);
    return parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

} // namespace bowl::json
