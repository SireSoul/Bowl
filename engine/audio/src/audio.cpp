#include "ph/audio/audio.hpp"

#include "ph/core/log.hpp"
#include "ph/core/math.hpp"

#include <SDL3/SDL.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ph::audio {

// --- Music ----------------------------------------------------------------------------------------------------------

std::unique_ptr<Music> Music::from_memory(std::vector<u8> ogg) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg.data(), static_cast<int>(ogg.size()), &err, nullptr);
    if (!v) return nullptr;
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    f32 seconds = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    if (vi.channels < 1 || vi.channels > 2 || vi.sample_rate == 0) return nullptr;
    return std::make_unique<Music>(std::make_shared<const std::vector<u8>>(std::move(ogg)), vi.sample_rate,
                                   static_cast<u32>(vi.channels), seconds);
}

// A playing music track: Vorbis decoder -> SDL resampler (to 48 kHz stereo) -> gain ramp -> mix.
struct AudioEngine::Stream {
    std::shared_ptr<const std::vector<u8>> data;
    stb_vorbis* decoder = nullptr;
    SDL_AudioStream* resampler = nullptr;
    u32 channels = 2;
    f32 gain = 0, target = 1;
    f32 ramp = 0; // gain change per output frame; 0 = jump
    bool loop = true, ended = false, stopping = false;
    Bus bus = Bus::Music;

    ~Stream() {
        if (decoder) stb_vorbis_close(decoder);
        if (resampler) SDL_DestroyAudioStream(resampler);
    }
};

static f32 ramp_per_frame(f32 from, f32 to, f32 seconds) {
    return seconds > 0 ? std::abs(to - from) / (seconds * static_cast<f32>(Sound::sample_rate)) : 0.0f;
}

void AudioEngine::play_music(const Music& music, const MusicParams& params) {
    auto s = std::make_unique<Stream>();
    s->data = music.data();
    int err = 0;
    s->decoder = stb_vorbis_open_memory(s->data->data(), static_cast<int>(s->data->size()), &err, nullptr);
    if (!s->decoder) {
        error("audio: music can't be decoded (stb_vorbis error {})", err);
        return;
    }
    s->channels = music.channels();
    SDL_AudioSpec src{SDL_AUDIO_F32, static_cast<int>(music.channels()), static_cast<int>(music.sample_rate())};
    SDL_AudioSpec dst{SDL_AUDIO_F32, static_cast<int>(Sound::channels), static_cast<int>(Sound::sample_rate)};
    s->resampler = SDL_CreateAudioStream(&src, &dst);
    if (!s->resampler) {
        error("audio: SDL_CreateAudioStream failed: {}", SDL_GetError());
        return;
    }
    s->loop = params.loop;
    s->bus = params.bus;
    s->target = params.gain;
    s->gain = params.fade_in > 0 ? 0.0f : params.gain;
    s->ramp = ramp_per_frame(0, params.gain, params.fade_in);

    std::lock_guard lock(mutex_);
    for (auto& old : streams_) { // crossfade out whatever was playing
        old->stopping = true;
        old->target = 0;
        old->ramp = ramp_per_frame(old->gain, 0, params.fade_in);
        if (old->ramp == 0) old->gain = 0;
    }
    streams_.push_back(std::move(s));
}

void AudioEngine::stop_music(f32 fade_out) {
    std::lock_guard lock(mutex_);
    for (auto& s : streams_) {
        s->stopping = true;
        s->target = 0;
        s->ramp = ramp_per_frame(s->gain, 0, fade_out);
        if (s->ramp == 0) s->gain = 0;
    }
}

bool AudioEngine::music_playing() const {
    std::lock_guard lock(mutex_);
    return std::any_of(streams_.begin(), streams_.end(), [](const auto& s) { return !s->stopping; });
}

// Called with mutex_ held.
void AudioEngine::mix_streams(f32* out, u32 frames) {
    f32 master = bus_gain_[static_cast<usize>(Bus::Master)];
    const int want = static_cast<int>(frames * Sound::channels * sizeof(f32));
    stream_buffer_.resize(frames * Sound::channels);
    std::vector<f32> decoded;
    for (auto& sp : streams_) {
        Stream& s = *sp;
        // Decode until the resampler holds enough output for this callback.
        int empty_reads = 0;
        while (!s.ended && SDL_GetAudioStreamAvailable(s.resampler) < want) {
            decoded.resize(2048 * s.channels);
            int got = stb_vorbis_get_samples_float_interleaved(s.decoder, static_cast<int>(s.channels), decoded.data(),
                                                               static_cast<int>(decoded.size()));
            if (got > 0) {
                SDL_PutAudioStreamData(s.resampler, decoded.data(), got * static_cast<int>(s.channels * sizeof(f32)));
                empty_reads = 0;
            } else if (s.loop && ++empty_reads < 2) {
                stb_vorbis_seek_start(s.decoder); // gapless: the resampler keeps its history
            } else {
                SDL_FlushAudioStream(s.resampler);
                s.ended = true;
            }
        }
        int bytes = SDL_GetAudioStreamData(s.resampler, stream_buffer_.data(), want);
        u32 got_frames = bytes > 0 ? static_cast<u32>(bytes) / (Sound::channels * sizeof(f32)) : 0;
        f32 bus = master * bus_gain_[static_cast<usize>(s.bus)];
        for (u32 i = 0; i < got_frames; ++i) {
            if (s.ramp > 0) {
                if (s.gain < s.target) s.gain = std::min(s.target, s.gain + s.ramp);
                else if (s.gain > s.target) s.gain = std::max(s.target, s.gain - s.ramp);
            } else {
                s.gain = s.target;
            }
            out[i * 2] += stream_buffer_[i * 2] * s.gain * bus;
            out[i * 2 + 1] += stream_buffer_[i * 2 + 1] * s.gain * bus;
        }
        if (s.ended && got_frames < frames) { // played out
            s.stopping = true;
            s.gain = 0;
        }
    }
    std::erase_if(streams_, [](const auto& s) { return s->stopping && s->gain <= 0.0f; });
}

std::unique_ptr<Sound> Sound::from_mono(std::span<const f32> mono) {
    std::vector<f32> stereo(mono.size() * 2);
    for (usize i = 0; i < mono.size(); ++i) stereo[i * 2] = stereo[i * 2 + 1] = mono[i];
    return std::make_unique<Sound>(std::move(stereo));
}

AudioEngine::AudioEngine() = default;
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
    streams_.clear();
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
    mix_streams(out, frames);
    // Soft clip instead of wrapping when many loud voices stack up.
    for (u32 i = 0; i < frames * 2; ++i) {
        f32 x = out[i];
        if (x > 1.0f || x < -1.0f) out[i] = std::tanh(x);
    }
}

namespace {

// Converts any PCM to the mixer format (48 kHz stereo float).
std::unique_ptr<Sound> to_mixer_format(std::string_view path, const SDL_AudioSpec& spec, const void* data, int bytes) {
    SDL_AudioSpec target{SDL_AUDIO_F32, static_cast<int>(Sound::channels), static_cast<int>(Sound::sample_rate)};
    Uint8* converted = nullptr;
    int converted_len = 0;
    if (!SDL_ConvertAudioSamples(&spec, static_cast<const Uint8*>(data), bytes, &target, &converted, &converted_len)) {
        log_asset_error(path, "Sound", SDL_GetError());
        return nullptr;
    }
    std::vector<f32> samples(static_cast<usize>(converted_len) / sizeof(f32));
    std::memcpy(samples.data(), converted, samples.size() * sizeof(f32));
    SDL_free(converted);
    return std::make_unique<Sound>(std::move(samples));
}

bool is_ogg(std::string_view path) { return path.ends_with(".ogg") || path.ends_with(".oga"); }

} // namespace

void register_asset_loaders(AssetManager& assets) {
    // Sound: decoded fully up front. Use for effects; use Music for long tracks.
    assets.register_loader<Sound>([](AssetLoadContext& ctx) -> std::unique_ptr<Sound> {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Sound", "file not found in any mount");
            return nullptr;
        }
        if (is_ogg(ctx.path())) {
            int channels = 0, rate = 0;
            short* pcm = nullptr;
            int frames = stb_vorbis_decode_memory(bytes->data(), static_cast<int>(bytes->size()), &channels, &rate, &pcm);
            if (frames <= 0 || !pcm) {
                log_asset_error(ctx.path(), "Sound", "not a valid Ogg Vorbis file");
                return nullptr;
            }
            SDL_AudioSpec spec{SDL_AUDIO_S16, channels, rate};
            auto sound = to_mixer_format(ctx.path(), spec, pcm, frames * channels * static_cast<int>(sizeof(short)));
            std::free(pcm);
            return sound;
        }
        SDL_IOStream* io = SDL_IOFromConstMem(bytes->data(), bytes->size());
        SDL_AudioSpec spec;
        Uint8* data = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV_IO(io, true, &spec, &data, &length)) {
            log_asset_error(ctx.path(), "Sound", SDL_GetError());
            return nullptr;
        }
        auto sound = to_mixer_format(ctx.path(), spec, data, static_cast<int>(length));
        SDL_free(data);
        return sound;
    });

    assets.register_loader<Music>([](AssetLoadContext& ctx) -> std::unique_ptr<Music> {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Music", "file not found in any mount");
            return nullptr;
        }
        auto music = Music::from_memory(std::move(*bytes));
        if (!music) log_asset_error(ctx.path(), "Music", "not a valid mono/stereo Ogg Vorbis file");
        return music;
    });
}

} // namespace ph::audio
