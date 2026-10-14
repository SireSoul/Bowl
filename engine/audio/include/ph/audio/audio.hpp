#pragma once

#include "ph/core/assets.hpp"
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

enum class Bus : u8 { Master, Music, Effects, Ambience, Voice, Ui };
inline constexpr usize bus_count = 6;

struct PlayParams {
    f32 gain = 1;
    f32 pan = 0;   // -1 left .. +1 right (equal-power)
    f32 pitch = 1; // playback rate
    bool loop = false;
    Bus bus = Bus::Effects;
};

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

    AudioEngine() = default;
    ~AudioEngine();

    bool init();
    void shutdown();
    bool active() const { return stream_ != nullptr; }

    Voice play(const Sound& sound, const PlayParams& params = {});
    Voice play(const Asset<Sound>& sound, const PlayParams& params = {}) {
        return sound ? play(*sound, params) : Voice{};
    }
    void stop(Voice voice);
    void stop_all();
    bool playing(Voice voice) const;
    void set_gain(Voice voice, f32 gain);
    void set_pan(Voice voice, f32 pan);
    void set_pitch(Voice voice, f32 pitch);

    void set_bus_gain(Bus bus, f32 gain);
    f32 bus_gain(Bus bus) const;
    u32 active_voices() const;

    // Renders `frames` stereo frames into out (used by the device callback; public for offline rendering/tests).
    void mix(f32* out, u32 frames);

private:
    struct VoiceState {
        std::shared_ptr<const std::vector<f32>> samples;
        f64 position = 0; // in frames
        PlayParams params;
        u32 generation = 0;
        bool active = false;
    };
    VoiceState* find(Voice v);
    const VoiceState* find(Voice v) const;

    mutable std::mutex mutex_;
    std::array<VoiceState, max_voices> voices_{};
    std::array<f32, bus_count> bus_gain_{1, 1, 1, 1, 1, 1};
    u32 next_generation_ = 1;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<f32> scratch_;
};

// Registers the Sound loader (WAV via SDL) with the asset manager.
void register_asset_loaders(AssetManager& assets);

} // namespace ph::audio
