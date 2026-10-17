#pragma once

#include "bowl/core/types.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace bowl {

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

namespace detail {
// Names that don't identify one type: every file's anonymous namespace prints the same, and so do functions with
// internal linkage that declare a type of the same name. (Clang leaves the function out of a local type's name
// altogether, so there it can't be seen.)
constexpr bool name_is_local(std::string_view name) {
    for (std::string_view marker : {"(anonymous namespace)", // Clang
                                    "{anonymous}",           // GCC
                                    "`anonymous namespace'", // MSVC
                                    ")::", ") const::",      // GCC: declared in a function, "f()::T"
                                    "'::"})                  // MSVC: "`void __cdecl f(void)'::`2'::T"
        if (name.find(marker) != std::string_view::npos) return true;
    return false;
}

// One per type, at its own address (it's writable, so identical-data folding never merges two). A type with a
// local name has one per file, which tells same-named ones apart; a named type has one per module.
template <class T> inline char type_anchor = 0;

// One per module (the executable, or a game library): hidden, so a library never binds to the host's.
#if defined(_WIN32)
inline char module_anchor = 0;
#else
[[gnu::visibility("hidden")]] inline char module_anchor = 0;
#endif
} // namespace detail

// Whether type_name<T>() fails to identify T on its own: it's in an anonymous namespace, or declared in a function.
template <class T> constexpr bool has_local_name() { return detail::name_is_local(type_name<T>()); }

// Identifies a type in maps (resources, asset loaders). Derived from the type's name, so it's the same in every
// module of the process (a hot-reloaded game library and its host agree on it) and across runs. Types with a local
// name (has_local_name) instead get an id of their own each time the process runs, so two files' private
// `namespace { struct Settings; }` stay apart. Clang can't show that a type was declared in a function, so give
// resource and asset types a namespace-scope name there.
using TypeId = u64;
template <class T> constexpr TypeId type_id() {
    using U = std::remove_cvref_t<T>;
    if constexpr (has_local_name<U>())
        return (type_hash<U>() ^ reinterpret_cast<std::uintptr_t>(&detail::type_anchor<U>)) * 0x100000001b3ull;
    else
        return type_hash<U>();
}

} // namespace bowl
