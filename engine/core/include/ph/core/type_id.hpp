#pragma once

#include "ph/core/types.hpp"

#include <string_view>
#include <type_traits>

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

// Identifies a type in maps (resources, asset loaders). Derived from the type's name, so it's the same in every
// module of the process (a hot-reloaded game library and its host agree on it) and across runs.
using TypeId = u64;
template <class T> constexpr TypeId type_id() { return type_hash<std::remove_cvref_t<T>>(); }

} // namespace ph
