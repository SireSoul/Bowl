#include "ph/audio/audio.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace ph;
using namespace ph::audio;

// The mixer runs without an audio device, so it is tested offline.

TEST_CASE("audio: a voice plays to the end and frees its slot") {
    AudioEngine audio;
    Sound sound(std::vector<f32>(2 * 100, 0.5f)); // 100 stereo frames
    Voice v = audio.play(sound, {.gain = 1.0f});
    CHECK(audio.playing(v));
    std::vector<f32> out(2 * 64);
    audio.mix(out.data(), 64);
    CHECK(out[0] == doctest::Approx(0.5f));
    CHECK(audio.playing(v));
    audio.mix(out.data(), 64);
    CHECK_FALSE(audio.playing(v));
    CHECK(audio.active_voices() == 0);
    CHECK(out[2 * 63] == 0.0f); // silence after the end
}

TEST_CASE("audio: pan, bus gain and looping") {
    AudioEngine audio;
    Sound sound(std::vector<f32>(2 * 10, 1.0f));
    audio.set_bus_gain(Bus::Music, 0.5f);
    Voice v = audio.play(sound, {.gain = 1.0f, .pan = -1.0f, .loop = true, .bus = Bus::Music});
    std::vector<f32> out(2 * 50);
    audio.mix(out.data(), 50);
    CHECK(out[0] == doctest::Approx(0.5f * 1.41421356f)); // hard left, equal-power
    CHECK(out[1] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(audio.playing(v)); // looping past the end
    audio.stop(v);
    CHECK_FALSE(audio.playing(v));
    audio.stop(v); // stale handle is harmless
}

TEST_CASE("audio: stale handles never touch a reused slot") {
    AudioEngine audio;
    Sound sound(std::vector<f32>(2 * 4, 1.0f));
    Voice first = audio.play(sound);
    audio.stop(first);
    Voice second = audio.play(sound);
    CHECK(second.slot == first.slot);
    audio.set_gain(first, 0.0f); // must not affect `second`
    std::vector<f32> out(2 * 1);
    audio.mix(out.data(), 1);
    CHECK(out[0] > 0.5f);
}
