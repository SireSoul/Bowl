#include "ph/core/ease.hpp"
#include "ph/core/serialize.hpp"
#include "ph/gfx/particles.hpp"

#include <doctest/doctest.h>

using namespace ph;
using namespace ph::gfx;

TEST_CASE("ease: endpoints, monotonic basics, overshoot where expected") {
    for (int e = 0; e <= static_cast<int>(Ease::InOutBounce); ++e) {
        auto curve = static_cast<Ease>(e);
        CHECK(ease(curve, 0) == doctest::Approx(0).epsilon(1e-4));
        CHECK(ease(curve, 1) == doctest::Approx(1).epsilon(1e-4));
    }
    CHECK(ease(Ease::OutQuad, 0.5f) > 0.5f);
    CHECK(ease(Ease::InQuad, 0.5f) < 0.5f);
    CHECK(ease(Ease::OutBack, 0.8f) > 1.0f); // overshoots
    CHECK(ease(Ease::Linear, 2.0f) == 1.0f); // clamped
}

TEST_CASE("tween: plays, finishes, reverses from where it is") {
    Tween<Vec2> t{{0, 0}, {100, 0}, 1.0f, Ease::Linear};
    CHECK(t.update(0.25f).x == doctest::Approx(25));
    CHECK_FALSE(t.done());
    t.reverse(); // now 25 -> 0
    CHECK(t.value().x == doctest::Approx(25));
    CHECK(t.update(0.5f).x == doctest::Approx(12.5f));
    t.update(1.0f);
    CHECK(t.done());
    CHECK(t.value().x == doctest::Approx(0));

    Tween<f32> delayed{0, 1, 1, Ease::Linear, 0, 0.5f};
    CHECK(delayed.update(0.4f) == 0.0f); // still waiting
    CHECK(delayed.update(0.6f) == doctest::Approx(0.5f));
}

TEST_CASE("particles: emission rate, lifetime, motion, cap") {
    ParticleSettings s;
    s.rate = 100;
    s.lifetime_min = s.lifetime_max = 0.5f;
    s.speed_min = s.speed_max = 10;
    s.direction = 0; // +x
    s.spread = 0;
    ParticleSystem p(42);
    for (int i = 0; i < 10; ++i) p.update(s, {0, 0}, 0.01f); // 0.1 s at 100/s
    CHECK(p.size() == doctest::Approx(10).epsilon(0.11));
    for (int i = 0; i < 60; ++i) p.update(s, {0, 0}, 0.01f, false); // stop emitting; all die after 0.5 s
    CHECK(p.size() == 0);

    s.max_particles = 50;
    p.burst(s, {0, 0}, 500);
    CHECK(p.size() == 50);
    p.clear();
    CHECK(p.size() == 0);
}

TEST_CASE("particles: same seed, same particles") {
    ParticleSettings s;
    s.spread = 3;
    ParticleSystem a(7), b(7);
    a.burst(s, {}, 20);
    b.burst(s, {}, 20);
    for (int i = 0; i < 5; ++i) {
        a.update(s, {}, 0.1f);
        b.update(s, {}, 0.1f);
    }
    CHECK(a.size() == b.size());
}

TEST_CASE("particle settings are reflected and serializable") {
    ParticleEmitter e;
    e.settings.rate = 12.5f;
    e.settings.color_start = Color::hex(0xff8800);
    serial::Context ctx;
    json::Value j = serial::to_json(e, ctx);
    CHECK(j["settings"]["rate"].as_number() == doctest::Approx(12.5));
    ParticleEmitter back;
    REQUIRE(serial::from_json(j, back, ctx));
    CHECK(back.settings.color_start == e.settings.color_start);
}
