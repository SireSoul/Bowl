#pragma once

#include "ph/core/assets.hpp"
#include "ph/core/math.hpp"
#include "ph/core/types.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

struct SDL_AudioStream;

namespace ph::audio {

// Decoded PCM: interleaved stereo float at the mixer rate. Shared, so a voice keeps playing the old data even if the
// asset is hot-reloaded underneath it.
class Sound {
public:
    static constexpr u32 sample_rate = 48000;
    static constexpr u32 channels = 2;

    explicit Sound(std::vector<f32> interleaved_stereo)
        : samples_(std::make_shared<const std::vector<f32>>(std::move(interleaved_stereo))) {}
    // Mono samples at sample_rate, e.g. synthesized effects.
    static std::unique_ptr<Sound> from_mono(std::span<const f32> mono);

    u32 frames() const { return static_cast<u32>(samples_->size() / channels); }
    f32 seconds() const { return static_cast<f32>(frames()) / sample_rate; }
    const std::shared_ptr<const std::vector<f32>>& samples() const { return samples_; }

private:
    std::shared_ptr<const std::vector<f32>> samples_;
};

// Compressed music (Ogg Vorbis, or Ogg Opus when built with PH_OPUS), decoded a little at a time while it plays.
// Loaded as an asset from .ogg and .opus files.
class Music {
public:
    enum class Codec : u8 { Vorbis, Opus };

    Music(std::shared_ptr<const std::vector<u8>> data, Codec codec, u32 sample_rate, u32 channels, f32 seconds)
        : data_(std::move(data)), codec_(codec), sample_rate_(sample_rate), channels_(channels), seconds_(seconds) {}
    // Validates and measures an Ogg Vorbis or Opus file in memory; null if it isn't one (or Opus isn't built in).
    static std::unique_ptr<Music> from_memory(std::vector<u8> ogg);

    const std::shared_ptr<const std::vector<u8>>& data() const { return data_; }
    Codec codec() const { return codec_; }
    u32 sample_rate() const { return sample_rate_; }
    u32 channels() const { return channels_; }
    f32 seconds() const { return seconds_; }

private:
    std::shared_ptr<const std::vector<u8>> data_;
    Codec codec_;
    u32 sample_rate_, channels_;
    f32 seconds_;
};

// Whether .opus files can be loaded (the engine was built with -DPH_OPUS=ON).
bool opus_supported();

enum class Bus : u8 { Master, Music, Effects, Ambience, Voice, Ui };
inline constexpr usize bus_count = 6;

// How loud a sound in the world is from where the listener is. Full volume within `near`, silent beyond `far`,
// fading smoothly between; panned fully to one side once it's `pan_distance` to the left or right.
struct Attenuation {
    f32 near = 64;
    f32 far = 1024;
    f32 pan_distance = 512;
};

struct PlayParams {
    f32 gain = 1;
    f32 pan = 0;   // -1 left .. +1 right (equal-power)
    f32 pitch = 1; // playback rate
    bool loop = false;
    Bus bus = Bus::Effects;
};

struct MusicParams {
    f32 gain = 1;
    f32 fade_in = 0;   // seconds; the previous track fades out over the same time (crossfade)
    bool loop = true;  // gapless
    Bus bus = Bus::Music;
};

// Effects on everything that plays through a bus, applied before the bus gain. Changes are smoothed, so they can
// follow gameplay (muffle the music while paused, add reverb in a cave).
struct BusEffects {
    f32 low_pass = 0;     // Hz: cut what's above (muffled, underwater: 400-1000). 0 = off
    f32 high_pass = 0;    // Hz: cut what's below (thin, a radio: 1000-2000). 0 = off
    f32 reverb = 0;       // 0..1, how much of the reverb is heard
    f32 room_size = 0.5f; // 0..1, how long the reverb rings
    f32 damping = 0.5f;   // 0..1, how quickly its highs die away
};

// The gain and pan for a sound at `source` heard from `listener`, both in world units.
struct Spatial {
    f32 gain = 1;
    f32 pan = 0;
};
Spatial spatialize(Vec2 source, Vec2 listener, const Attenuation& attenuation = {});

// Handle to a playing sound. Stale handles (sound already finished) are ignored safely.
struct Voice {
    u32 slot = 0;
    u32 generation = 0;
    explicit operator bool() const { return generation != 0; }
};

// Software mixer on SDL's audio device. Every method is thread-safe; mixing happens on SDL's audio thread.
class AudioEngine : NonCopyable {
public:
    static constexpr u32 max_voices = 128;

    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();
    bool active() const { return stream_ != nullptr; }

    Voice play(const Sound& sound, const PlayParams& params = {});
    Voice play(const Asset<Sound>& sound, const PlayParams& params = {}) {
        return sound ? play(*sound, params) : Voice{};
    }
    // A sound at a place in the world: its gain and pan follow the listener as either moves. params.pan is ignored.
    Voice play_at(const Sound& sound, Vec2 position, const PlayParams& params = {}, const Attenuation& attenuation = {});
    Voice play_at(const Asset<Sound>& sound, Vec2 position, const PlayParams& params = {},
                  const Attenuation& attenuation = {}) {
        return sound ? play_at(*sound, position, params, attenuation) : Voice{};
    }
    void stop(Voice voice);
    void stop_all();
    bool playing(Voice voice) const;
    void set_gain(Voice voice, f32 gain);
    void set_pan(Voice voice, f32 pan);
    void set_pitch(Voice voice, f32 pitch);
    // Moves a sound started with play_at.
    void set_position(Voice voice, Vec2 position);

    // Where the player hears from, usually the camera or the player character. Set it every frame.
    void set_listener(Vec2 position);
    Vec2 listener() const;

    // Streams a music track, replacing (crossfading from) whatever music is playing.
    void play_music(const Music& music, const MusicParams& params = {});
    void play_music(const Asset<Music>& music, const MusicParams& params = {}) {
        if (music) play_music(*music, params);
    }
    void stop_music(f32 fade_out = 0);
    bool music_playing() const;

    void set_bus_gain(Bus bus, f32 gain);
    f32 bus_gain(Bus bus) const;
    void set_bus_effects(Bus bus, const BusEffects& effects);
    BusEffects bus_effects(Bus bus) const;
    u32 active_voices() const;

    // Renders `frames` stereo frames into out (used by the device callback; public for offline rendering/tests).
    void mix(f32* out, u32 frames);

private:
    struct Stream;  // a playing music track (decoder + resampler)
    struct Effects; // a bus's filters and reverb
    void mix_streams(u32 frames);
    struct VoiceState {
        std::shared_ptr<const std::vector<f32>> samples;
        f64 position = 0; // in frames
        PlayParams params;
        u32 generation = 0;
        bool active = false;
        bool spatial = false;
        Vec2 world_position;
        Attenuation attenuation;
    };
    VoiceState* find(Voice v);
    const VoiceState* find(Voice v) const;
    Voice start(const Sound& sound, const PlayParams& params); // with mutex_ held

    mutable std::mutex mutex_;
    std::array<VoiceState, max_voices> voices_{};
    std::array<f32, bus_count> bus_gain_{1, 1, 1, 1, 1, 1};
    std::array<BusEffects, bus_count> bus_effects_{};
    std::array<std::unique_ptr<Effects>, bus_count> effects_;
    std::array<std::vector<f32>, bus_count> bus_buffers_;
    Vec2 listener_;
    u32 next_generation_ = 1;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<f32> scratch_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::vector<f32> stream_buffer_;
};

// Registers the Sound (WAV, OGG, Opus) and Music (OGG, Opus; streamed) loaders with the asset manager.
void register_asset_loaders(AssetManager& assets);

} // namespace ph::audio
