#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/core/json.hpp"
#include "bowl/core/reflect.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bowl::serial {

// State shared across one save or load.
struct Context {
    AssetManager* assets = nullptr;                     // to reload Asset<T> fields by path
    std::unordered_map<u64, u64>* entity_map = nullptr; // runtime entity <-> file id (see ecs/world_io.hpp)
    std::vector<std::string> warnings;                  // problems that didn't stop the load
    // The game's own save version, written into world files as "game_version" when above 0 (see ecs::Migrations).
    i32 game_version = 0;
    void warn(std::string message) { warnings.push_back(std::move(message)); }
};

// What can be saved: bool, numbers, enums (as numbers), std::string, Vec2/3/4, Color, Rect, std::vector, std::array,
// std::set (as a list), std::map with string keys (as an object), std::optional, Asset<T> (as its path), reflected structs (as objects), and any type with ADL hooks
//     json::Value bowl_to_json(const T&, serial::Context&);
//     bool bowl_from_json(const json::Value&, T&, serial::Context&);
// Loading is tolerant: missing keys keep the current value, so adding fields never breaks old save files.

template <class T> json::Value to_json(const T& value, Context& ctx);
template <class T> bool from_json(const json::Value& j, T& value, Context& ctx);

template <class T> struct is_vector : std::false_type {};
template <class T, class A> struct is_vector<std::vector<T, A>> : std::true_type {};
template <class T> struct is_array : std::false_type {};
template <class T, usize N> struct is_array<std::array<T, N>> : std::true_type {};
template <class T> struct is_set : std::false_type {};
template <class T, class C, class A> struct is_set<std::set<T, C, A>> : std::true_type {};
template <class T> struct is_string_map : std::false_type {};
template <class T, class C, class A> struct is_string_map<std::map<std::string, T, C, A>> : std::true_type {};
template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_asset : std::false_type {};
template <class T> struct is_asset<Asset<T>> : std::true_type {};

template <class T>
concept HasHooks = requires(const T& c, T& m, const json::Value& j, Context& ctx) {
    { bowl_to_json(c, ctx) } -> std::convertible_to<json::Value>;
    { bowl_from_json(j, m, ctx) } -> std::convertible_to<bool>;
};

template <class T>
concept Serializable = std::is_same_v<T, json::Value> || std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                       std::is_same_v<T, std::string> ||
                       std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4> ||
                       std::is_same_v<T, Color> || std::is_same_v<T, Rect> || is_vector<T>::value ||
                       is_array<T>::value || is_set<T>::value || is_string_map<T>::value || is_optional<T>::value || is_asset<T>::value || reflect::Reflected<T> ||
                       HasHooks<T>;

namespace detail {
template <usize N> json::Value floats(const f32 (&v)[N]) {
    json::Value a;
    for (f32 x : v) a.push(x);
    return a;
}
template <usize N> bool read_floats(const json::Value& j, f32 (&v)[N]) {
    if (!j.is_array() || j.size() != N) return false;
    for (usize i = 0; i < N; ++i) v[i] = j[i].as_float(v[i]);
    return true;
}
} // namespace detail

template <class T> json::Value to_json(const T& v, Context& ctx) {
    static_assert(Serializable<T>, "this type can't be serialized: reflect it with BOWL_REFLECT or add bowl_to_json/"
                                   "bowl_from_json hooks");
    if constexpr (HasHooks<T>) return bowl_to_json(v, ctx);
    else if constexpr (std::is_same_v<T, json::Value>) return v;
    else if constexpr (is_string_map<T>::value) {
        json::Value o;
        o.make_object();
        for (const auto& [key, item] : v) o.set(key, to_json(item, ctx));
        return o;
    }
    else if constexpr (std::is_same_v<T, bool>) return v;
    else if constexpr (std::is_enum_v<T>) return static_cast<i64>(v);
    else if constexpr (std::is_arithmetic_v<T>) return static_cast<f64>(v);
    else if constexpr (std::is_same_v<T, std::string>) return v;
    else if constexpr (std::is_same_v<T, Vec2>) return detail::floats<2>({v.x, v.y});
    else if constexpr (std::is_same_v<T, Vec3>) return detail::floats<3>({v.x, v.y, v.z});
    else if constexpr (std::is_same_v<T, Vec4>) return detail::floats<4>({v.x, v.y, v.z, v.w});
    else if constexpr (std::is_same_v<T, Color>) return detail::floats<4>({v.r, v.g, v.b, v.a});
    else if constexpr (std::is_same_v<T, Rect>) return detail::floats<4>({v.x, v.y, v.w, v.h});
    else if constexpr (is_vector<T>::value || is_array<T>::value || is_set<T>::value) {
        json::Value a;
        a.make_array();
        for (const auto& item : v) a.push(to_json(item, ctx));
        return a;
    } else if constexpr (is_optional<T>::value) {
        return v ? to_json(*v, ctx) : json::Value{};
    } else if constexpr (is_asset<T>::value) {
        return v ? json::Value(v.path()) : json::Value{};
    } else {
        json::Value o;
        o.make_object();
        reflect::for_each_field(v, [&](std::string_view name, const auto& field) { o.set(name, to_json(field, ctx)); });
        return o;
    }
}

template <class T> bool from_json(const json::Value& j, T& v, Context& ctx) {
    static_assert(Serializable<T>, "this type can't be deserialized: reflect it with BOWL_REFLECT or add hooks");
    if constexpr (HasHooks<T>) return bowl_from_json(j, v, ctx);
    else if constexpr (std::is_same_v<T, json::Value>) {
        v = j;
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!j.is_bool()) return false;
        v = j.as_bool();
        return true;
    } else if constexpr (std::is_enum_v<T>) {
        if (!j.is_number()) return false;
        v = static_cast<T>(j.as_int());
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        if (!j.is_number() || std::isnan(j.as_number())) return false;
        // Clamped to the field's range: a file can hold any number, and converting one that doesn't fit is undefined.
        f64 n = j.as_number();
        if (n <= static_cast<f64>(std::numeric_limits<T>::min())) v = std::numeric_limits<T>::min();
        else if (n >= static_cast<f64>(std::numeric_limits<T>::max())) v = std::numeric_limits<T>::max();
        else v = static_cast<T>(n);
        return true;
    } else if constexpr (std::is_arithmetic_v<T>) {
        if (!j.is_number()) return false;
        v = static_cast<T>(j.as_number());
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!j.is_string()) return false;
        v = std::string(j.as_string());
        return true;
    } else if constexpr (std::is_same_v<T, Vec2>) {
        f32 f[2] = {v.x, v.y};
        if (!detail::read_floats(j, f)) return false;
        v = {f[0], f[1]};
        return true;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        f32 f[3] = {v.x, v.y, v.z};
        if (!detail::read_floats(j, f)) return false;
        v = {f[0], f[1], f[2]};
        return true;
    } else if constexpr (std::is_same_v<T, Vec4> || std::is_same_v<T, Color> || std::is_same_v<T, Rect>) {
        f32 f[4];
        std::memcpy(f, &v, sizeof f);
        if (!detail::read_floats(j, f)) return false;
        std::memcpy(&v, f, sizeof f);
        return true;
    } else if constexpr (is_vector<T>::value) {
        if (!j.is_array()) return false;
        v.clear();
        v.resize(j.size());
        bool ok = true;
        for (usize i = 0; i < j.size(); ++i) ok = from_json(j[i], v[i], ctx) && ok;
        return ok;
    } else if constexpr (is_set<T>::value) {
        if (!j.is_array()) return false;
        v.clear();
        bool ok = true;
        for (const json::Value& item : j.array()) {
            typename T::value_type x{};
            if (from_json(item, x, ctx)) v.insert(std::move(x));
            else ok = false;
        }
        return ok;
    } else if constexpr (is_string_map<T>::value) {
        if (!j.is_object()) return false;
        v.clear();
        bool ok = true;
        for (const auto& [key, item] : j.object()) {
            typename T::mapped_type x{};
            if (from_json(item, x, ctx)) v.emplace(key, std::move(x));
            else ok = false;
        }
        return ok;
    } else if constexpr (is_array<T>::value) {
        if (!j.is_array()) return false;
        bool ok = j.size() == v.size();
        for (usize i = 0; i < v.size() && i < j.size(); ++i) ok = from_json(j[i], v[i], ctx) && ok;
        return ok;
    } else if constexpr (is_optional<T>::value) {
        if (j.is_null()) {
            v.reset();
            return true;
        }
        if (!v) v.emplace();
        return from_json(j, *v, ctx);
    } else if constexpr (is_asset<T>::value) {
        if (j.is_null()) {
            v = {};
            return true;
        }
        if (!j.is_string() || !ctx.assets) return false;
        using Inner = std::remove_cvref_t<decltype(*v)>;
        v = ctx.assets->load<Inner>(j.as_string());
        return true;
    } else {
        if (!j.is_object()) return false;
        bool ok = true;
        reflect::for_each_field(v, [&](std::string_view name, auto& field) {
            const json::Value* item = j.find(name);
            if (!item) return; // missing: keep the default
            if (!from_json(*item, field, ctx)) {
                ctx.warn(std::format("field '{}' of {} has the wrong type; kept its current value", name,
                                     reflect::info<T>().name));
                ok = false;
            }
        });
        return ok;
    }
}

} // namespace bowl::serial
