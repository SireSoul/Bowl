#pragma once

#include "bowl/core/math.hpp"

#include <cmath>

namespace bowl {

// Easing curves: t in 0..1 -> 0..1 (some overshoot, like back and elastic).
enum class Ease : u8 {
    Linear,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InSine, OutSine, InOutSine,
    InExpo, OutExpo, InOutExpo,
    InBack, OutBack, InOutBack,
    InElastic, OutElastic, InOutElastic,
    InBounce, OutBounce, InOutBounce,
};

namespace detail {
inline f32 out_bounce(f32 t) {
    constexpr f32 n = 7.5625f, d = 2.75f;
    if (t < 1 / d) return n * t * t;
    if (t < 2 / d) { t -= 1.5f / d; return n * t * t + 0.75f; }
    if (t < 2.5f / d) { t -= 2.25f / d; return n * t * t + 0.9375f; }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}
} // namespace detail

inline f32 ease(Ease e, f32 t) {
    t = saturate(t);
    constexpr f32 c1 = 1.70158f, c2 = c1 * 1.525f, c3 = c1 + 1;
    constexpr f32 c4 = tau / 3, c5 = tau / 4.5f;
    switch (e) {
    case Ease::Linear: return t;
    case Ease::InQuad: return t * t;
    case Ease::OutQuad: return 1 - (1 - t) * (1 - t);
    case Ease::InOutQuad: return t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2.0f) / 2;
    case Ease::InCubic: return t * t * t;
    case Ease::OutCubic: return 1 - std::pow(1 - t, 3.0f);
    case Ease::InOutCubic: return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
    case Ease::InQuart: return t * t * t * t;
    case Ease::OutQuart: return 1 - std::pow(1 - t, 4.0f);
    case Ease::InOutQuart: return t < 0.5f ? 8 * t * t * t * t : 1 - std::pow(-2 * t + 2, 4.0f) / 2;
    case Ease::InSine: return 1 - std::cos(t * pi / 2);
    case Ease::OutSine: return std::sin(t * pi / 2);
    case Ease::InOutSine: return -(std::cos(pi * t) - 1) / 2;
    case Ease::InExpo: return t == 0 ? 0 : std::pow(2.0f, 10 * t - 10);
    case Ease::OutExpo: return t == 1 ? 1 : 1 - std::pow(2.0f, -10 * t);
    case Ease::InOutExpo:
        if (t == 0 || t == 1) return t;
        return t < 0.5f ? std::pow(2.0f, 20 * t - 10) / 2 : (2 - std::pow(2.0f, -20 * t + 10)) / 2;
    case Ease::InBack: return c3 * t * t * t - c1 * t * t;
    case Ease::OutBack: return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
    case Ease::InOutBack:
        return t < 0.5f ? (std::pow(2 * t, 2.0f) * ((c2 + 1) * 2 * t - c2)) / 2
                        : (std::pow(2 * t - 2, 2.0f) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
    case Ease::InElastic:
        if (t == 0 || t == 1) return t;
        return -std::pow(2.0f, 10 * t - 10) * std::sin((t * 10 - 10.75f) * c4);
    case Ease::OutElastic:
        if (t == 0 || t == 1) return t;
        return std::pow(2.0f, -10 * t) * std::sin((t * 10 - 0.75f) * c4) + 1;
    case Ease::InOutElastic:
        if (t == 0 || t == 1) return t;
        return t < 0.5f ? -(std::pow(2.0f, 20 * t - 10) * std::sin((20 * t - 11.125f) * c5)) / 2
                        : (std::pow(2.0f, -20 * t + 10) * std::sin((20 * t - 11.125f) * c5)) / 2 + 1;
    case Ease::InBounce: return 1 - detail::out_bounce(1 - t);
    case Ease::OutBounce: return detail::out_bounce(t);
    case Ease::InOutBounce:
        return t < 0.5f ? (1 - detail::out_bounce(1 - 2 * t)) / 2 : (1 + detail::out_bounce(2 * t - 1)) / 2;
    }
    return t;
}

// Animates a value from `from` to `to` over `duration` seconds. Works for anything lerp() supports (f32, Vec2,
// Color, ...). Plain data: keep it in a component and step it in a system.
//
//     Tween<Vec2> slide{{0, 0}, {200, 0}, 0.4f, Ease::OutBack};
//     panel.position = slide.update(dt);
template <class T> struct Tween {
    T from{}, to{};
    f32 duration = 1;
    Ease curve = Ease::Linear;
    f32 elapsed = 0;
    f32 delay = 0;

    T value() const {
        f32 t = duration > 0 ? (elapsed - delay) / duration : 1.0f;
        return lerp(from, to, ease(curve, t));
    }
    T update(f32 dt) {
        elapsed += dt;
        return value();
    }
    bool done() const { return elapsed >= delay + duration; }
    void restart() { elapsed = 0; }
    // Plays backwards from wherever it is now (e.g. a panel closing halfway through opening).
    void reverse() {
        T current = value();
        to = from;
        from = current;
        elapsed = delay;
    }
};

} // namespace bowl
