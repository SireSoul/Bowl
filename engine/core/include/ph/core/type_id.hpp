#pragma once

#include "ph/core/types.hpp"

#include <string_view>

namespace ph {

// Compile-time type name, e.g. type_name<game::Health>() == "game::Health". Used for debugging and the inspector.
template <class T> constexpr std::string_view type_name() {
#if defined(_MSC_VER) && !defined(__clang__)
    std::string_view s = __FUNCSIG__;
    constexpr std::string_view open = "type_name<";
    constexpr std::string_view close = ">(void)";
#else
    std::string_view s = __PRETTY_FUNCTION__;
    constexpr std::string_view open = "T = ";
#if defined(__clang__)
    constexpr std::string_view close = "]";
#else
    constexpr std::string_view close = ";";
#endif
#endif
    s.remove_prefix(s.find(open) + open.size());
    s = s.substr(0, s.rfind(close));
    // MSVC prefixes "struct " / "class " / "enum ".
    for (std::string_view prefix : {"struct ", "class ", "enum "})
        if (s.starts_with(prefix)) s.remove_prefix(prefix.size());
    return s;
}

constexpr u64 fnv1a(std::string_view s) {
    u64 h = 0xcbf29ce484222325ull;
    for (char c : s) h = (h ^ static_cast<u8>(c)) * 0x100000001b3ull;
    return h;
}

// Stable 64-bit id derived from the type's name: identical across runs and builds of the same code.
template <class T> constexpr u64 type_hash() { return fnv1a(type_name<T>()); }

// Small dense per-process id, assigned on first use. Good for array indices; not stable across runs.
using TypeId = u32;
namespace detail {
TypeId next_type_id();
}
template <class T> TypeId type_id() {
    static const TypeId id = detail::next_type_id();
    return id;
}

} // namespace ph
