#include "ph/audio/audio.hpp"

#include "ph/core/log.hpp"
#include "ph/core/math.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ph::audio {

std::unique_ptr<Sound> Sound::from_mono(std::span<const f32> mono) {
    std::vector<f32> stereo(mono.size() * 2);
    for (usize i = 0; i < mono.size(); ++i) stereo[i * 2] = stereo[i * 2 + 1] = mono[i];
    return std::make_unique<Sound>(std::move(stereo));
}

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init() {
    SDL_AudioSpec spec{SDL_AUDIO_F32, static_cast<int>(Sound::channels), static_cast<int>(Sound::sample_rate)};
    auto callback = [](void* user, SDL_AudioStream* stream, int additional, int) {
        auto* self = static_cast<AudioEngine*>(user);
        u32 frames = static_cast<u32>(additional) / (sizeof(f32) * Sound::channels);
        if (frames == 0) return;
        self->scratch_.resize(frames * Sound::channels);
        self->mix(self->scratch_.data(), frames);
        SDL_PutAudioStreamData(stream, self->scratch_.data(), static_cast<int>(self->scratch_.size() * sizeof(f32)));
    };
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
    if (!stream_) {
        warn("audio: no output device ({}); running silent", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    info("audio: {} Hz stereo, {} voices", Sound::sample_rate, max_voices);
    return true;
}

void AudioEngine::shutdown() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    std::lock_guard lock(mutex_);
    for (auto& v : voices_) v = {};
}

AudioEngine::VoiceState* AudioEngine::find(Voice v) {
    if (!v || v.slot >= max_voices) return nullptr;
    VoiceState& s = voices_[v.slot];
    return s.active && s.generation == v.generation ? &s : nullptr;
}

const AudioEngine::VoiceState* AudioEngine::find(Voice v) const {
    return const_cast<AudioEngine*>(this)->find(v);
}

Voice AudioEngine::play(const Sound& sound, const PlayParams& params) {
    std::lock_guard lock(mutex_);
    // Reuse a free slot; if all are busy, steal the quietest non-looping voice.
    u32 slot = max_voices;
    for (u32 i = 0; i < max_voices; ++i)
        if (!voices_[i].active) { slot = i; break; }
    if (slot == max_voices) {
        f32 quietest = 1e9f;
        for (u32 i = 0; i < max_voices; ++i)
            if (!voices_[i].params.loop && voices_[i].params.gain < quietest) { quietest = voices_[i].params.gain; slot = i; }
        if (slot == max_voices) return {};
    }
    VoiceState& v = voices_[slot];
    v.samples = sound.samples();
    v.position = 0;
    v.params = params;
    v.generation = next_generation_++;
    if (next_generation_ == 0) next_generation_ = 1;
    v.active = true;
    return {slot, v.generation};
}

void AudioEngine::stop(Voice voice) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) *v = {};
}

void AudioEngine::stop_all() {
    std::lock_guard lock(mutex_);
    for (auto& v : voices_) v = {};
}

bool AudioEngine::playing(Voice voice) const {
    std::lock_guard lock(mutex_);
    return find(voice) != nullptr;
}

void AudioEngine::set_gain(Voice voice, f32 gain) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) v->params.gain = gain;
}

void AudioEngine::set_pan(Voice voice, f32 pan) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) v->params.pan = std::clamp(pan, -1.0f, 1.0f);
}

void AudioEngine::set_pitch(Voice voice, f32 pitch) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) v->params.pitch = std::max(pitch, 0.01f);
}

void AudioEngine::set_bus_gain(Bus bus, f32 gain) {
    std::lock_guard lock(mutex_);
    bus_gain_[static_cast<usize>(bus)] = std::max(gain, 0.0f);
}

f32 AudioEngine::bus_gain(Bus bus) const {
    std::lock_guard lock(mutex_);
    return bus_gain_[static_cast<usize>(bus)];
}

u32 AudioEngine::active_voices() const {
    std::lock_guard lock(mutex_);
    return static_cast<u32>(std::count_if(voices_.begin(), voices_.end(), [](const VoiceState& v) { return v.active; }));
}

void AudioEngine::mix(f32* out, u32 frames) {
    std::memset(out, 0, frames * Sound::channels * sizeof(f32));
    std::lock_guard lock(mutex_);
    f32 master = bus_gain_[static_cast<usize>(Bus::Master)];
    for (VoiceState& v : voices_) {
        if (!v.active) continue;
        const std::vector<f32>& s = *v.samples;
        const f64 length = static_cast<f64>(s.size() / 2);
        if (length < 1) { v = {}; continue; }
        f32 gain = v.params.gain * master * bus_gain_[static_cast<usize>(v.params.bus)];
        f32 angle = (std::clamp(v.params.pan, -1.0f, 1.0f) + 1.0f) * (pi / 4.0f);
        f32 gl = gain * std::cos(angle) * std::numbers::sqrt2_v<f32>;
        f32 gr = gain * std::sin(angle) * std::numbers::sqrt2_v<f32>;
        f64 step = v.params.pitch;
        for (u32 i = 0; i < frames; ++i) {
            if (v.position >= length) {
                if (!v.params.loop) { v = {}; break; }
                v.position = std::fmod(v.position, length);
            }
            // Linear interpolation between neighbouring frames (pitch != 1 resamples).
            auto i0 = static_cast<usize>(v.position);
            usize i1 = i0 + 1 < static_cast<usize>(length) ? i0 + 1 : (v.params.loop ? 0 : i0);
            f32 t = static_cast<f32>(v.position - static_cast<f64>(i0));
            f32 l = s[i0 * 2] + (s[i1 * 2] - s[i0 * 2]) * t;
            f32 r = s[i0 * 2 + 1] + (s[i1 * 2 + 1] - s[i0 * 2 + 1]) * t;
            out[i * 2] += l * gl;
            out[i * 2 + 1] += r * gr;
            v.position += step;
        }
    }
    // Soft clip instead of wrapping when many loud voices stack up.
    for (u32 i = 0; i < frames * 2; ++i) {
        f32 x = out[i];
        if (x > 1.0f || x < -1.0f) out[i] = std::tanh(x);
    }
}

void register_asset_loaders(AssetManager& assets) {
    assets.register_loader<Sound>([](AssetLoadContext& ctx) -> std::unique_ptr<Sound> {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Sound", "file not found in any mount");
            return nullptr;
        }
        SDL_IOStream* io = SDL_IOFromConstMem(bytes->data(), bytes->size());
        SDL_AudioSpec spec;
        Uint8* data = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV_IO(io, true, &spec, &data, &length)) {
            log_asset_error(ctx.path(), "Sound", SDL_GetError());
            return nullptr;
        }
        SDL_AudioSpec target{SDL_AUDIO_F32, static_cast<int>(Sound::channels), static_cast<int>(Sound::sample_rate)};
        Uint8* converted = nullptr;
        int converted_len = 0;
        bool ok = SDL_ConvertAudioSamples(&spec, data, static_cast<int>(length), &target, &converted, &converted_len);
        SDL_free(data);
        if (!ok) {
            log_asset_error(ctx.path(), "Sound", SDL_GetError());
            return nullptr;
        }
        std::vector<f32> samples(static_cast<usize>(converted_len) / sizeof(f32));
        std::memcpy(samples.data(), converted, samples.size() * sizeof(f32));
        SDL_free(converted);
        return std::make_unique<Sound>(std::move(samples));
    });
}

} // namespace ph::audio
