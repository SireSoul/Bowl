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

// Bus effects and positional sound

namespace {

std::unique_ptr<Sound> sine(f32 hz, f32 seconds, f32 amplitude = 0.5f) {
    std::vector<f32> mono(static_cast<usize>(seconds * Sound::sample_rate));
    for (usize i = 0; i < mono.size(); ++i)
        mono[i] = amplitude * std::sin(2 * pi * hz * static_cast<f32>(i) / Sound::sample_rate);
    return Sound::from_mono(mono);
}

f32 peak_of(const std::vector<f32>& v, usize from = 0) {
    f32 p = 0;
    for (usize i = from; i < v.size(); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

} // namespace

TEST_CASE("audio: a low-pass bus muffles high notes and leaves low ones") {
    auto high = sine(6000, 0.5f), low = sine(100, 0.5f);
    std::vector<f32> out(2 * 4800);
    for (int pass = 0; pass < 2; ++pass) {
        AudioEngine audio;
        audio.set_bus_effects(Bus::Effects, {.low_pass = 500});
        audio.play(pass == 0 ? *high : *low);
        audio.mix(out.data(), 4800);
        audio.mix(out.data(), 4800); // the cutoff has glided down by now
        if (pass == 0) CHECK(peak_of(out) < 0.05f);
        else CHECK(peak_of(out) > 0.4f);
    }
    AudioEngine audio;
    audio.set_bus_effects(Bus::Effects, {.high_pass = 2000});
    audio.play(*low);
    audio.mix(out.data(), 4800);
    audio.mix(out.data(), 4800);
    CHECK(peak_of(out) < 0.05f);
    CHECK(audio.bus_effects(Bus::Effects).high_pass == 2000);
}

TEST_CASE("audio: reverb rings on after the sound stops") {
    auto blip = sine(440, 0.05f);
    std::vector<f32> out(2 * 4800);
    AudioEngine dry, wet;
    wet.set_bus_effects(Bus::Effects, {.reverb = 0.6f, .room_size = 0.8f});
    dry.play(*blip);
    wet.play(*blip);
    dry.mix(out.data(), 4800);
    wet.mix(out.data(), 4800);
    dry.mix(out.data(), 4800);
    CHECK(peak_of(out) == 0.0f);
    wet.mix(out.data(), 4800); // 100-200 ms later: only the reverb is left
    CHECK(peak_of(out) > 0.001f);
}

TEST_CASE("audio: positional sounds fade with distance and pan to their side") {
    Attenuation a{.near = 100, .far = 1000, .pan_distance = 500};
    CHECK(spatialize({50, 0}, {}, a).gain == 1.0f);
    CHECK(spatialize({2000, 0}, {}, a).gain == 0.0f);
    f32 mid = spatialize({550, 0}, {}, a).gain;
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    CHECK(spatialize({-600, 0}, {}, a).pan == -1.0f);
    CHECK(spatialize({250, 0}, {}, a).pan == doctest::Approx(0.5f));

    AudioEngine audio;
    Sound loud(std::vector<f32>(2 * 2000, 1.0f));
    Voice v = audio.play_at(loud, {400, 0}, {.loop = true}, {.near = 0, .far = 800, .pan_distance = 400});
    std::vector<f32> out(2 * 16);
    audio.mix(out.data(), 16);
    CHECK(out[0] == doctest::Approx(0.0f).epsilon(1e-4)); // fully right
    CHECK(out[1] > 0.3f);
    audio.set_listener({400, 0}); // walk up to it
    audio.mix(out.data(), 16);
    CHECK(out[0] == doctest::Approx(out[1]));
    CHECK(out[0] == doctest::Approx(1.0f).epsilon(0.01));
    audio.set_position(v, {400, 900}); // it moves out of earshot
    audio.mix(out.data(), 16);
    CHECK(peak_of(out) == 0.0f);
}

#if PH_OPUS
#include <ogg/ogg.h>
#include <opus.h>

namespace {

// An Ogg Opus file made in memory: a stereo tone, 440 Hz left and 660 Hz right.
std::vector<u8> make_opus(f32 seconds) {
    int error = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    REQUIRE(enc);
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    ogg_stream_state os;
    ogg_stream_init(&os, 1);
    std::vector<u8> file;
    auto write_pages = [&](bool flush) {
        ogg_page page;
        while (flush ? ogg_stream_flush(&os, &page) : ogg_stream_pageout(&os, &page)) {
            file.insert(file.end(), page.header, page.header + page.header_len);
            file.insert(file.end(), page.body, page.body + page.body_len);
        }
    };
    auto packet = [&](std::vector<u8> data, ogg_int64_t granule, long number, bool bos, bool eos) {
        ogg_packet p{data.data(), static_cast<long>(data.size()), bos ? 1L : 0L, eos ? 1L : 0L, granule, number};
        ogg_stream_packetin(&os, &p);
    };
    std::vector<u8> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2,
                            static_cast<u8>(lookahead), static_cast<u8>(lookahead >> 8), 0x80, 0xBB, 0, 0, 0, 0, 0};
    packet(head, 0, 0, true, false);
    write_pages(true);
    std::vector<u8> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 4, 0, 0, 0, 'b', 'o', 'w', 'l', 0, 0, 0, 0};
    packet(tags, 0, 1, false, false);
    write_pages(true);

    const int frame = 960; // 20 ms
    int frames = static_cast<int>(seconds * 48000) / frame;
    std::vector<f32> pcm(frame * 2);
    std::vector<u8> out(4000);
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < frame; ++i) {
            f32 t = static_cast<f32>(f * frame + i) / 48000.0f;
            pcm[i * 2] = 0.5f * std::sin(2 * pi * 440 * t);
            pcm[i * 2 + 1] = 0.5f * std::sin(2 * pi * 660 * t);
        }
        int bytes = opus_encode_float(enc, pcm.data(), frame, out.data(), static_cast<opus_int32>(out.size()));
        REQUIRE(bytes > 0);
        packet(std::vector<u8>(out.begin(), out.begin() + bytes), lookahead + (f + 1) * frame, f + 2, false,
               f == frames - 1);
        write_pages(false);
    }
    write_pages(true);
    ogg_stream_clear(&os);
    opus_encoder_destroy(enc);
    return file;
}

} // namespace

TEST_CASE("audio: Opus sounds and music") {
    CHECK(opus_supported());
    std::vector<u8> file = make_opus(0.5f);
    auto path = std::filesystem::temp_directory_path() / "ph_test_tone.opus";
    REQUIRE(write_file_bytes(path, file.data(), file.size()));
    AssetManager assets;
    assets.mount("", path.parent_path());
    register_asset_loaders(assets);
    auto sound = assets.load<Sound>("ph_test_tone.opus");
    REQUIRE(sound);
    CHECK(sound->seconds() == doctest::Approx(0.5).epsilon(0.02));
    CHECK(frequency(*sound->samples(), 0, 4000, 16000) == doctest::Approx(440).epsilon(0.03));
    CHECK(frequency(*sound->samples(), 1, 4000, 16000) == doctest::Approx(660).epsilon(0.03));

    auto music = Music::from_memory(file);
    REQUIRE(music);
    CHECK(music->codec() == Music::Codec::Opus);
    CHECK(music->seconds() == doctest::Approx(0.5).epsilon(0.02));
    AudioEngine audio;
    audio.play_music(*music, {.loop = false});
    std::vector<f32> chunk(2 * 480), all;
    for (int i = 0; i < 200 && audio.music_playing(); ++i) {
        audio.mix(chunk.data(), 480);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    CHECK_FALSE(audio.music_playing());
    CHECK(frequency(all, 1, 4000, 16000) == doctest::Approx(660).epsilon(0.03));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
#else
TEST_CASE("audio: without PH_OPUS, Opus files are refused clearly") { CHECK_FALSE(opus_supported()); }
#endif
