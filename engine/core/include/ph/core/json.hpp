#pragma once

#include "ph/core/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ph::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // keeps file order, so saved files diff cleanly

// A JSON document node. Lookups never throw: a missing key or a wrong type gives a null Value or the fallback you
// pass, so optional fields read in one expression: sheet["meta"]["size"]["w"].as_int(0)
class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : data_(b) {}
    Value(f64 n) : data_(n) {}
    Value(int n) : data_(static_cast<f64>(n)) {}
    Value(i64 n) : data_(static_cast<f64>(n)) {}
    Value(u32 n) : data_(static_cast<f64>(n)) {}
    Value(f32 n) : data_(static_cast<f64>(n)) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}
    Value(std::string_view s) : data_(std::string(s)) {}
    Value(Array a) : data_(std::move(a)) {}
    Value(Object o) : data_(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
    bool is_bool() const { return std::holds_alternative<bool>(data_); }
    bool is_number() const { return std::holds_alternative<f64>(data_); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_array() const { return std::holds_alternative<Array>(data_); }
    bool is_object() const { return std::holds_alternative<Object>(data_); }

    bool as_bool(bool fallback = false) const { return is_bool() ? std::get<bool>(data_) : fallback; }
    f64 as_number(f64 fallback = 0) const { return is_number() ? std::get<f64>(data_) : fallback; }
    f32 as_float(f32 fallback = 0) const { return static_cast<f32>(as_number(fallback)); }
    i64 as_int(i64 fallback = 0) const { return is_number() ? static_cast<i64>(std::get<f64>(data_)) : fallback; }
    std::string_view as_string(std::string_view fallback = {}) const {
        return is_string() ? std::string_view(std::get<std::string>(data_)) : fallback;
    }

    // Arrays and objects. Out-of-range or missing gives a shared null.
    usize size() const;
    const Value& operator[](usize index) const;
    const Value& operator[](std::string_view key) const;
    const Value* find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    const Array& array() const;
    const Object& object() const;

    // Building and changing documents. make_array/make_object keep the contents when the value already is one.
    Array& make_array();
    Object& make_object();
    Value& set(std::string_view key, Value value); // adds or replaces
    void push(Value value) { make_array().push_back(std::move(value)); }
    Value* find(std::string_view key);
    bool erase(std::string_view key); // false if there was no such key

    bool operator==(const Value&) const = default;

private:
    std::variant<std::monostate, bool, f64, std::string, Array, Object> data_;
};

struct ParseError {
    std::string message;
    u32 line = 0, column = 0;
};

struct ParseResult {
    Value value;
    std::optional<ParseError> error;
    explicit operator bool() const { return !error; }
};

// Strict JSON, plus two conveniences for hand-written files: // and /* */ comments, and trailing commas.
ParseResult parse(std::string_view text);

// indent < 0: compact one line; otherwise pretty-printed with that many spaces.
std::string stringify(const Value& value, int indent = 2);

// A compact binary form of a document, for big saves: the same values, with every key and string stored once and
// numbers in as few bytes as they need. Several times smaller than text, and quicker to read. Starts "PHJB".
std::vector<u8> encode_binary(const Value& value);
ParseResult decode_binary(std::span<const u8> data); // errors give the byte offset as the column
bool is_binary(std::span<const u8> data);
// Reads either form.
ParseResult parse_any(std::span<const u8> data);

} // namespace ph::json
