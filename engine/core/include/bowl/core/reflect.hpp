#pragma once

#include "bowl/core/assets.hpp"
#include "bowl/core/math.hpp"
#include "bowl/core/type_id.hpp"
#include "bowl/core/types.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// Reflection without code generation. After a struct, at namespace scope (in the struct's own namespace):
//
//     struct Health { i32 current = 100; i32 max = 100; bool invulnerable = false; };
//     BOWL_REFLECT(Health, current, max, invulnerable)
//
// Reflected types serialize to JSON, save and load with the world, and are editable live in the dev overlay.
// Up to 32 fields per type.

namespace bowl::reflect {

template <class C, class M> struct Field {
    std::string_view name;
    M C::*member;
};

template <class... Fields> struct TypeInfo {
    std::string_view name;
    std::tuple<Fields...> fields;
};
template <class... Fields> TypeInfo(std::string_view, std::tuple<Fields...>) -> TypeInfo<Fields...>;

// Live editing (the dev overlay's inspector) walks values through this interface; values are passed by reference
// and changes are written back. Unsupported field types are reported by name so they still show up.
class FieldVisitor {
public:
    virtual ~FieldVisitor() = default;
    virtual void boolean(std::string_view name, bool& value) = 0;
    virtual void integer(std::string_view name, i64& value, i64 min, i64 max) = 0;
    virtual void real(std::string_view name, f64& value) = 0;
    virtual void text(std::string_view name, std::string& value) = 0;
    virtual void vector(std::string_view name, f32* values, int count) = 0;
    virtual void color(std::string_view name, Color& value) = 0;
    virtual void entity(std::string_view name, u64 bits) = 0; // read-only
    // The same, for visitors that change entity references (the ECS swaps stand-in ids for real ones this way).
    virtual void entity_ref(std::string_view name, u64& bits) { entity(name, bits); }
    virtual bool begin_group(std::string_view name) = 0;      // false: skip its fields
    virtual void end_group() = 0;
    virtual void unsupported(std::string_view name, std::string_view type) = 0;
    // Asset<T> fields. `path` is the asset's path ("" for none); change it and return true to load that path instead,
    // which needs assets() to return the manager to load from.
    virtual bool asset(std::string_view name, std::string& path, std::string_view type) {
        (void)path;
        unsupported(name, type);
        return false;
    }
    virtual AssetManager* assets() { return nullptr; }
    // std::vector and std::array fields. `size` is how many items there are; a visitor may change it for resizable
    // lists (vectors) to add or remove items at the end. Return false to skip the items.
    virtual bool begin_list(std::string_view name, usize& size, bool resizable) {
        (void)size;
        (void)resizable;
        unsupported(name, "list");
        return false;
    }
    virtual void end_list() {}
};

namespace detail {
template <class T> struct list_traits {
    static constexpr bool is_list = false;
};
template <class T, class A> struct list_traits<std::vector<T, A>> {
    static constexpr bool is_list = true;
    static constexpr bool resizable = std::is_default_constructible_v<T>;
};
template <class T, usize N> struct list_traits<std::array<T, N>> {
    static constexpr bool is_list = true;
    static constexpr bool resizable = false;
};
} // namespace detail

template <class T> void visit(FieldVisitor& v, std::string_view name, T& value);

// Found by argument-dependent lookup: BOWL_REFLECT defines bowl_reflect_info(const T*) next to T.
template <class T>
concept Reflected = requires(const T* p) { bowl_reflect_info(p); };

template <Reflected T> constexpr auto info() { return bowl_reflect_info(static_cast<const T*>(nullptr)); }

// Calls f(name, member) for each reflected field of obj (const or not).
template <class T, class F>
    requires Reflected<std::remove_const_t<T>>
void for_each_field(T& obj, F&& f) {
    std::apply([&](const auto&... field) { (f(field.name, obj.*(field.member)), ...); },
               info<std::remove_const_t<T>>().fields);
}

template <Reflected T> constexpr usize field_count() { return std::tuple_size_v<decltype(info<T>().fields)>; }

// Types that know how to show themselves (e.g. ecs::Entity) provide bowl_visit(FieldVisitor&, name, T&) via ADL.
template <class T>
concept HasVisitHook = requires(FieldVisitor& v, std::string_view n, T& t) { bowl_visit(v, n, t); };

template <class T> void visit(FieldVisitor& v, std::string_view name, T& value) {
    if constexpr (HasVisitHook<T>) {
        bowl_visit(v, name, value);
    } else if constexpr (std::is_same_v<T, bool>) {
        v.boolean(name, value);
    } else if constexpr (std::is_integral_v<T>) {
        i64 x = static_cast<i64>(value);
        constexpr i64 lo = std::is_signed_v<T> ? static_cast<i64>(std::numeric_limits<T>::min()) : 0;
        constexpr i64 hi = sizeof(T) >= 8 ? std::numeric_limits<i64>::max() : static_cast<i64>(std::numeric_limits<T>::max());
        v.integer(name, x, lo, hi);
        value = static_cast<T>(x);
    } else if constexpr (std::is_enum_v<T>) {
        i64 x = static_cast<i64>(value);
        v.integer(name, x, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max());
        value = static_cast<T>(x);
    } else if constexpr (std::is_floating_point_v<T>) {
        f64 x = value;
        v.real(name, x);
        value = static_cast<T>(x);
    } else if constexpr (std::is_same_v<T, std::string>) {
        v.text(name, value);
    } else if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4> ||
                         std::is_same_v<T, Rect>) {
        v.vector(name, reinterpret_cast<f32*>(&value), static_cast<int>(sizeof(T) / sizeof(f32)));
    } else if constexpr (std::is_same_v<T, Color>) {
        v.color(name, value);
    } else if constexpr (bowl::is_asset_handle<T>::value) {
        std::string path(value.path());
        if (v.asset(name, path, type_name<typename T::value_type>())) {
            if (path.empty()) value = {};
            else if (AssetManager* assets = v.assets()) value = assets->template load<typename T::value_type>(path);
        }
    } else if constexpr (detail::list_traits<T>::is_list) {
        usize size = value.size();
        if (v.begin_list(name, size, detail::list_traits<T>::resizable)) {
            if constexpr (detail::list_traits<T>::resizable)
                if (size != value.size()) value.resize(size);
            for (usize i = 0; i < value.size(); ++i) {
                if constexpr (std::is_same_v<T, std::vector<bool>>) { // its items are proxies, not bools
                    bool item = value[i];
                    visit(v, "[" + std::to_string(i) + "]", item);
                    value[i] = item;
                } else {
                    visit(v, "[" + std::to_string(i) + "]", value[i]);
                }
            }
            v.end_list();
        }
    } else if constexpr (Reflected<T>) {
        if (v.begin_group(name)) {
            for_each_field(value, [&](std::string_view field, auto& member) { visit(v, field, member); });
            v.end_group();
        }
    } else {
        v.unsupported(name, type_name<T>());
    }
}

} // namespace bowl::reflect

#define BOWL_EXPAND(x) x
#define BOWL_FE_1(m, x) m(x)
#define BOWL_FE_2(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_1(m, __VA_ARGS__))
#define BOWL_FE_3(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_2(m, __VA_ARGS__))
#define BOWL_FE_4(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_3(m, __VA_ARGS__))
#define BOWL_FE_5(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_4(m, __VA_ARGS__))
#define BOWL_FE_6(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_5(m, __VA_ARGS__))
#define BOWL_FE_7(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_6(m, __VA_ARGS__))
#define BOWL_FE_8(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_7(m, __VA_ARGS__))
#define BOWL_FE_9(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_8(m, __VA_ARGS__))
#define BOWL_FE_10(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_9(m, __VA_ARGS__))
#define BOWL_FE_11(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_10(m, __VA_ARGS__))
#define BOWL_FE_12(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_11(m, __VA_ARGS__))
#define BOWL_FE_13(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_12(m, __VA_ARGS__))
#define BOWL_FE_14(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_13(m, __VA_ARGS__))
#define BOWL_FE_15(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_14(m, __VA_ARGS__))
#define BOWL_FE_16(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_15(m, __VA_ARGS__))
#define BOWL_FE_17(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_16(m, __VA_ARGS__))
#define BOWL_FE_18(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_17(m, __VA_ARGS__))
#define BOWL_FE_19(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_18(m, __VA_ARGS__))
#define BOWL_FE_20(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_19(m, __VA_ARGS__))
#define BOWL_FE_21(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_20(m, __VA_ARGS__))
#define BOWL_FE_22(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_21(m, __VA_ARGS__))
#define BOWL_FE_23(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_22(m, __VA_ARGS__))
#define BOWL_FE_24(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_23(m, __VA_ARGS__))
#define BOWL_FE_25(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_24(m, __VA_ARGS__))
#define BOWL_FE_26(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_25(m, __VA_ARGS__))
#define BOWL_FE_27(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_26(m, __VA_ARGS__))
#define BOWL_FE_28(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_27(m, __VA_ARGS__))
#define BOWL_FE_29(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_28(m, __VA_ARGS__))
#define BOWL_FE_30(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_29(m, __VA_ARGS__))
#define BOWL_FE_31(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_30(m, __VA_ARGS__))
#define BOWL_FE_32(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_31(m, __VA_ARGS__))
#define BOWL_FE_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, NAME, ...) NAME
#define BOWL_FOR_EACH(m, ...) BOWL_EXPAND(BOWL_FE_PICK(__VA_ARGS__, BOWL_FE_32, BOWL_FE_31, BOWL_FE_30, BOWL_FE_29, BOWL_FE_28, BOWL_FE_27, BOWL_FE_26, BOWL_FE_25, BOWL_FE_24, BOWL_FE_23, BOWL_FE_22, BOWL_FE_21, BOWL_FE_20, BOWL_FE_19, BOWL_FE_18, BOWL_FE_17, BOWL_FE_16, BOWL_FE_15, BOWL_FE_14, BOWL_FE_13, BOWL_FE_12, BOWL_FE_11, BOWL_FE_10, BOWL_FE_9, BOWL_FE_8, BOWL_FE_7, BOWL_FE_6, BOWL_FE_5, BOWL_FE_4, BOWL_FE_3, BOWL_FE_2, BOWL_FE_1)(m, __VA_ARGS__))

#define BOWL_REFLECT_FIELD(f) \
    ::bowl::reflect::Field<BowlReflectedType, decltype(BowlReflectedType::f)>{#f, &BowlReflectedType::f}

#define BOWL_REFLECT(Type, ...)                                                                                        \
    [[maybe_unused]] inline constexpr auto bowl_reflect_info(const Type*) {                                            \
        using BowlReflectedType = Type;                                                                                \
        return ::bowl::reflect::TypeInfo{#Type,                                                                        \
                                         std::make_tuple(BOWL_FOR_EACH(BOWL_REFLECT_FIELD, __VA_ARGS__))};             \
    }
