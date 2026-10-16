// JSON text and binary documents: settings, saves, maps, sprite sheets and everything else a player can replace.
// Also TextDocument, which tools use to edit files people wrote by hand.

#include "fuzz.hpp"

#include "bowl/core/json.hpp"
#include "bowl/core/json_text.hpp"

#include <cmath>

using namespace bowl;

namespace {

bool finite(const json::Value& v) {
    if (v.is_number()) return std::isfinite(v.as_number());
    for (const json::Value& item : v.array())
        if (!finite(item)) return false;
    for (const auto& [key, item] : v.object())
        if (!finite(item)) return false;
    return true;
}

// The first path into the document, going into the first member or item at each level.
json::TextDocument::Path first_path(const json::Value& v) {
    json::TextDocument::Path path;
    for (const json::Value* at = &v;;) {
        if (at->is_object() && at->size()) {
            path.emplace_back(at->object()[0].first);
            at = &at->object()[0].second;
        } else if (at->is_array() && at->size()) {
            path.emplace_back(usize{0});
            at = &(*at)[0];
        } else {
            return path;
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const u8> bytes(data, size);
    json::ParseResult doc = json::parse_any(bytes);
    if (doc) {
        // Whatever reads, writes back and reads the same (binary documents can hold NaN, which equals nothing, and
        // text writes infinities as null).
        auto binary = json::decode_binary(json::encode_binary(doc.value));
        auto text = json::parse(json::stringify(doc.value));
        BOWL_FUZZ_CHECK(binary);
        BOWL_FUZZ_CHECK(text);
        if (finite(doc.value)) BOWL_FUZZ_CHECK(binary.value == doc.value && text.value == doc.value);
    }
    if (json::is_binary(bytes)) return 0;

    json::TextDocument edit;
    if (!edit.load(std::string(reinterpret_cast<const char*>(data), size))) return 0;
    json::TextDocument::Path path = first_path(edit.value());
    json::Value small;
    small.set("x", 1);
    if (!path.empty()) {
        edit.set(path, small);
        BOWL_FUZZ_CHECK(edit.valid());
        path.pop_back();
    }
    edit.insert(path, "added", "key");
    BOWL_FUZZ_CHECK(edit.valid());
    path = first_path(edit.value());
    if (!path.empty()) {
        edit.erase(path);
        BOWL_FUZZ_CHECK(edit.valid());
    }
    return 0;
}
