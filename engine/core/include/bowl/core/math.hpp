#pragma once

#include "bowl/core/types.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bowl {

inline constexpr f32 pi = std::numbers::pi_v<f32>;
inline constexpr f32 tau = 2.0f * pi;

constexpr f32 radians(f32 degrees) { return degrees * (pi / 180.0f); }
constexpr f32 degrees(f32 radians) { return radians * (180.0f / pi); }
template <class T> constexpr T lerp(T a, T b, f32 t) { return a + (b - a) * t; }
template <class T> constexpr T clamp(T v, T lo, T hi) { return std::clamp(v, lo, hi); }
constexpr f32 saturate(f32 v) { return std::clamp(v, 0.0f, 1.0f); }

struct Vec2 {
    f32 x = 0, y = 0;

    constexpr Vec2() = default;
    constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
    constexpr explicit Vec2(f32 s) : x(s), y(s) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(Vec2 o) const { return {x * o.x, y * o.y}; }
    constexpr Vec2 operator/(Vec2 o) const { return {x / o.x, y / o.y}; }
    constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(f32 s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }
    constexpr Vec2& operator/=(f32 s) { x /= s; y /= s; return *this; }
    constexpr bool operator==(const Vec2&) const = default;

    constexpr f32 dot(Vec2 o) const { return x * o.x + y * o.y; }
    constexpr f32 cross(Vec2 o) const { return x * o.y - y * o.x; }
    constexpr f32 length_squared() const { return dot(*this); }
    f32 length() const { return std::sqrt(length_squared()); }
    Vec2 normalized() const { f32 l = length(); return l > 0 ? *this / l : Vec2{}; }
    f32 distance(Vec2 o) const { return (o - *this).length(); }
    f32 angle() const { return std::atan2(y, x); }
    constexpr Vec2 perpendicular() const { return {-y, x}; }
    Vec2 rotated(f32 angle) const {
        f32 c = std::cos(angle), s = std::sin(angle);
        return {x * c - y * s, x * s + y * c};
    }
    static Vec2 from_angle(f32 angle) { return {std::cos(angle), std::sin(angle)}; }
};
constexpr Vec2 operator*(f32 s, Vec2 v) { return v * s; }

struct Vec3 {
    f32 x = 0, y = 0, z = 0;

    constexpr Vec3() = default;
    constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3(Vec2 v, f32 z_) : x(v.x), y(v.y), z(z_) {}
    constexpr explicit Vec3(f32 s) : x(s), y(s), z(s) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3&) const = default;

    constexpr f32 dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    constexpr f32 length_squared() const { return dot(*this); }
    f32 length() const { return std::sqrt(length_squared()); }
    Vec3 normalized() const { f32 l = length(); return l > 0 ? *this / l : Vec3{}; }
    constexpr Vec2 xy() const { return {x, y}; }
};

struct Vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;

    constexpr Vec4() = default;
    constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

    constexpr Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr Vec4 operator-(Vec4 o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    constexpr Vec4 operator*(f32 s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr bool operator==(const Vec4&) const = default;
    constexpr f32 dot(Vec4 o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
    constexpr Vec3 xyz() const { return {x, y, z}; }
};

// Column-major 4x4 matrix (matches GLSL / std140), for column vectors: v' = M * v.
struct Mat4 {
    Vec4 cols[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static constexpr Mat4 identity() { return {}; }

    constexpr Vec4 row(int r) const {
        auto at = [r](const Vec4& c) { return r == 0 ? c.x : r == 1 ? c.y : r == 2 ? c.z : c.w; };
        return {at(cols[0]), at(cols[1]), at(cols[2]), at(cols[3])};
    }
    constexpr Mat4 operator*(const Mat4& o) const {
        Mat4 m;
        for (int c = 0; c < 4; ++c) {
            Vec4 oc = o.cols[c];
            m.cols[c] = {row(0).dot(oc), row(1).dot(oc), row(2).dot(oc), row(3).dot(oc)};
        }
        return m;
    }
    constexpr Vec4 operator*(Vec4 v) const { return {row(0).dot(v), row(1).dot(v), row(2).dot(v), row(3).dot(v)}; }
    constexpr Vec2 transform_point(Vec2 p) const {
        Vec4 r = *this * Vec4{p.x, p.y, 0, 1};
        return {r.x, r.y};
    }

    static constexpr Mat4 translation(Vec3 t) {
        Mat4 m;
        m.cols[3] = {t.x, t.y, t.z, 1};
        return m;
    }
    static constexpr Mat4 scale(Vec3 s) {
        Mat4 m;
        m.cols[0].x = s.x;
        m.cols[1].y = s.y;
        m.cols[2].z = s.z;
        return m;
    }
    static Mat4 rotation_z(f32 angle) {
        Mat4 m;
        f32 c = std::cos(angle), s = std::sin(angle);
        m.cols[0] = {c, s, 0, 0};
        m.cols[1] = {-s, c, 0, 0};
        return m;
    }
    // Orthographic projection to SDL_GPU clip space (x,y in [-1,1] with +y up, z in [0,1]).
    static constexpr Mat4 ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near_z = -1, f32 far_z = 1) {
        Mat4 m;
        m.cols[0] = {2 / (right - left), 0, 0, 0};
        m.cols[1] = {0, 2 / (top - bottom), 0, 0};
        m.cols[2] = {0, 0, 1 / (far_z - near_z), 0};
        m.cols[3] = {-(right + left) / (right - left), -(top + bottom) / (top - bottom), -near_z / (far_z - near_z), 1};
        return m;
    }
    constexpr const f32* data() const { return &cols[0].x; }
};

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;

    static constexpr Rect from_center(Vec2 center, Vec2 size) {
        return {center.x - size.x * 0.5f, center.y - size.y * 0.5f, size.x, size.y};
    }
    constexpr Rect expanded(f32 by) const { return {x - by, y - by, w + 2 * by, h + 2 * by}; }
    constexpr Vec2 position() const { return {x, y}; }
    constexpr Vec2 size() const { return {w, h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr f32 right() const { return x + w; }
    constexpr f32 bottom() const { return y + h; }
    constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    constexpr bool overlaps(const Rect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
    constexpr bool operator==(const Rect&) const = default;
};

// RGBA with float channels, normally 0..1. Values above 1 are allowed and make sprites glow when bloom is on.
// Color::hex(0xRRGGBB) and Color::rgba8 convert from the usual 8-bit notations.
struct Color {
    f32 r = 1, g = 1, b = 1, a = 1;

    static constexpr Color rgba8(u8 red, u8 green, u8 blue, u8 alpha = 255) {
        return {red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f};
    }
    static constexpr Color hex(u32 rgb, f32 alpha = 1.0f) {
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
    }
    constexpr Color with_alpha(f32 alpha) const { return {r, g, b, alpha}; }
    constexpr Color premultiplied() const { return {r * a, g * a, b * a, a}; }
    constexpr bool operator==(const Color&) const = default;

    static constexpr Color white() { return {1, 1, 1, 1}; }
    static constexpr Color black() { return {0, 0, 0, 1}; }
    static constexpr Color transparent() { return {0, 0, 0, 0}; }
};
constexpr Color lerp(Color a, Color b, f32 t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

} // namespace bowl
