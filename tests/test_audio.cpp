#include "ph/audio/audio.hpp"
#include "ph/core/file.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
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

// Ogg Vorbis (fixtures: tests/data/README.md)

namespace {

// Rising zero crossings per second on one channel of interleaved stereo at the mixer rate.
f32 frequency(const std::vector<f32>& stereo, int channel, usize first_frame, usize frames) {
    int crossings = 0;
    for (usize i = first_frame + 1; i < first_frame + frames; ++i)
        if (stereo[(i - 1) * 2 + channel] < 0 && stereo[i * 2 + channel] >= 0) ++crossings;
    return static_cast<f32>(crossings) * Sound::sample_rate / static_cast<f32>(frames);
}

AssetManager& test_assets() {
    static AssetManager assets;
    static bool once = [] {
        assets.mount("", PH_TEST_DATA_DIR);
        register_asset_loaders(assets);
        return true;
    }();
    (void)once;
    return assets;
}

} // namespace

TEST_CASE("audio: OGG sounds decode and resample to 48 kHz stereo") {
    auto stereo = test_assets().load<Sound>("tone_44k_stereo.ogg");
    REQUIRE(stereo);
    CHECK(stereo->seconds() == doctest::Approx(0.5).epsilon(0.02));
    const auto& s = *stereo->samples();
    CHECK(frequency(s, 0, 2000, 18000) == doctest::Approx(440).epsilon(0.03));
    CHECK(frequency(s, 1, 2000, 18000) == doctest::Approx(660).epsilon(0.03));

    auto mono = test_assets().load<Sound>("tone_22k_mono.ogg");
    REQUIRE(mono);
    CHECK(mono->seconds() == doctest::Approx(0.25).epsilon(0.03));
    const auto& m = *mono->samples();
    CHECK(frequency(m, 0, 1000, 9000) == doctest::Approx(440).epsilon(0.03));
    CHECK(m[5000 * 2] == doctest::Approx(m[5000 * 2 + 1])); // mono lands on both channels
}

TEST_CASE("audio: music streams, ends, loops, stops") {
    auto music = test_assets().load<Music>("tone_44k_stereo.ogg");
    REQUIRE(music);
    CHECK(music->sample_rate() == 44100);
    CHECK(music->channels() == 2);
    CHECK(music->seconds() == doctest::Approx(0.5).epsilon(0.02));

    AudioEngine audio; // offline: no device, we pull the mix ourselves
    std::vector<f32> chunk(2 * 480);
    std::vector<f32> all;

    audio.play_music(music, {.loop = false});
    CHECK(audio.music_playing());
    for (int i = 0; i < 200 && audio.music_playing(); ++i) {
        audio.mix(chunk.data(), 480);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    CHECK_FALSE(audio.music_playing());
    usize frames = all.size() / 2;
    CHECK(frames == doctest::Approx(24000).epsilon(0.05)); // played once: 0.5 s
    CHECK(frequency(all, 0, 2000, 18000) == doctest::Approx(440).epsilon(0.03));

    audio.play_music(music, {.loop = true});
    for (int i = 0; i < 300; ++i) audio.mix(chunk.data(), 480); // 3 s: six times through
    CHECK(audio.music_playing());
    f32 peak = 0;
    for (f32 x : chunk) peak = std::max(peak, std::abs(x));
    CHECK(peak > 0.2f); // still sounding after many loops

    audio.stop_music(0.05f);
    for (int i = 0; i < 10; ++i) audio.mix(chunk.data(), 480);
    CHECK_FALSE(audio.music_playing());
    audio.mix(chunk.data(), 480);
    for (f32 x : chunk) CHECK(x == 0.0f);
}

TEST_CASE("audio: crossfading to a new track") {
    auto a = test_assets().load<Music>("tone_44k_stereo.ogg");
    auto b = test_assets().load<Music>("tone_22k_mono.ogg");
    AudioEngine audio;
    std::vector<f32> chunk(2 * 480);
    audio.play_music(a);
    for (int i = 0; i < 10; ++i) audio.mix(chunk.data(), 480);
    audio.play_music(b, {.fade_in = 0.1f});
    for (int i = 0; i < 20; ++i) audio.mix(chunk.data(), 480); // past the 0.1 s crossfade
    CHECK(audio.music_playing());
    // Only the new mono track remains: both channels carry the same signal.
    for (usize i = 0; i < 480; i += 37) CHECK(chunk[i * 2] == doctest::Approx(chunk[i * 2 + 1]).epsilon(0.001));
}
