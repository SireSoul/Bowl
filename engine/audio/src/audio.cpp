#include "bowl/audio/audio.hpp"

#include "decoders.hpp"

#include "bowl/core/log.hpp"
#include "bowl/core/math.hpp"

#include <SDL3/SDL.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include <stb_vorbis.c>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bowl::audio {

// Decoders

namespace detail {

std::optional<StreamInfo> probe_vorbis(const std::vector<u8>& data) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data.data(), static_cast<int>(data.size()), &err, nullptr);
    if (!v) return std::nullopt;
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    f32 seconds = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    if (vi.channels < 1 || vi.channels > 2 || vi.sample_rate == 0) return std::nullopt;
    return StreamInfo{vi.sample_rate, static_cast<u32>(vi.channels), seconds};
}

namespace {
class VorbisDecoder final : public Decoder {
public:
    explicit VorbisDecoder(stb_vorbis* v) : v_(v), channels_(static_cast<u32>(stb_vorbis_get_info(v).channels)) {}
    ~VorbisDecoder() override { stb_vorbis_close(v_); }
    u32 read(f32* out, u32 frames) override {
        int got = stb_vorbis_get_samples_float_interleaved(v_, static_cast<int>(channels_), out,
                                                           static_cast<int>(frames * channels_));
        return got > 0 ? static_cast<u32>(got) : 0;
    }
    void rewind() override { stb_vorbis_seek_start(v_); }

private:
    stb_vorbis* v_;
    u32 channels_;
};
} // namespace

std::unique_ptr<Decoder> open_vorbis(const std::vector<u8>& data) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(data.data(), static_cast<int>(data.size()), &err, nullptr);
    return v ? std::make_unique<VorbisDecoder>(v) : nullptr;
}

} // namespace detail

bool opus_supported() { return detail::opus_built(); }

// Music

std::unique_ptr<Music> Music::from_memory(std::vector<u8> ogg) {
    bool opus = detail::looks_like_opus(ogg);
    auto info = opus ? detail::probe_opus(ogg) : detail::probe_vorbis(ogg);
    if (!info) return nullptr;
    return std::make_unique<Music>(std::make_shared<const std::vector<u8>>(std::move(ogg)),
                                   opus ? Codec::Opus : Codec::Vorbis, info->sample_rate, info->channels, info->seconds);
}

// Positional sound

Spatial spatialize(Vec2 source, Vec2 listener, const Attenuation& a) {
    f32 d = source.distance(listener);
    Spatial s;
    if (d >= a.far) s.gain = 0;
    else if (d > a.near) {
        f32 t = (d - a.near) / std::max(a.far - a.near, 1e-3f);
        s.gain = (1 - t) * (1 - t); // fades quickly at first, then gently out to `far`
    }
    s.pan = a.pan_distance > 0 ? std::clamp((source.x - listener.x) / a.pan_distance, -1.0f, 1.0f) : 0.0f;
    return s;
}

// Bus effects

namespace {

// A state-variable filter (Cytomic's trapezoidal form): stays stable while its cutoff moves.
struct Svf {
    f32 ic1 = 0, ic2 = 0;
    void tick(f32 x, f32 a1, f32 a2, f32 a3, f32& low, f32& band) {
        f32 v3 = x - ic2;
        f32 v1 = a1 * ic1 + a2 * v3;
        f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        low = v2;
        band = v1;
    }
};

struct SvfCoefficients {
    f32 a1, a2, a3;
    static constexpr f32 k = std::numbers::sqrt2_v<f32>; // 1/Q, Q = 0.707: no resonant bump
    explicit SvfCoefficients(f32 cutoff) {
        f32 g = std::tan(pi * std::clamp(cutoff, 10.0f, 0.45f * Sound::sample_rate) / Sound::sample_rate);
        a1 = 1 / (1 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

// Freeverb (Jezar at Dreampoint, public domain): eight combs and four allpasses per side.
struct Comb {
    std::vector<f32> buffer;
    usize at = 0;
    f32 store = 0;
    f32 tick(f32 x, f32 feedback, f32 damp) {
        f32 out = buffer[at];
        store = out * (1 - damp) + store * damp;
        if (std::abs(store) < 1e-20f) store = 0;
        buffer[at] = x + store * feedback;
        if (++at == buffer.size()) at = 0;
        return out;
    }
};

struct Allpass {
    std::vector<f32> buffer;
    usize at = 0;
    f32 tick(f32 x) {
        f32 held = buffer[at];
        buffer[at] = x + held * 0.5f;
        if (++at == buffer.size()) at = 0;
        return held - x;
    }
};

} // namespace

struct AudioEngine::Effects {
    f32 low_pass = 20000, high_pass = 10; // current cutoffs, gliding toward the targets
    Svf lp[2], hp[2];
    bool reverb_ready = false;
    f32 wet = 0;
    Comb combs[2][8];
    Allpass allpasses[2][4];

    void prepare_reverb() {
        static constexpr u32 comb_lengths[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr u32 allpass_lengths[4] = {556, 441, 341, 225};
        constexpr f32 scale = Sound::sample_rate / 44100.0f; // tunings are for 44.1 kHz
        for (int side = 0; side < 2; ++side) {
            u32 spread = side ? 23 : 0; // the right side is slightly longer: a wider image
            for (int i = 0; i < 8; ++i) combs[side][i] = {std::vector<f32>(static_cast<usize>((comb_lengths[i] + spread) * scale), 0.0f)};
            for (int i = 0; i < 4; ++i)
                allpasses[side][i] = {std::vector<f32>(static_cast<usize>((allpass_lengths[i] + spread) * scale), 0.0f)};
        }
        reverb_ready = true;
    }

    static f32 glide(f32 current, f32 target, u32 frames) {
        // About 50 ms to settle, moving in octaves rather than hertz.
        f32 t = std::min(1.0f, static_cast<f32>(frames) / (0.05f * Sound::sample_rate));
        return std::exp(std::log(current) + (std::log(target) - std::log(current)) * t);
    }

    void process(f32* buf, u32 frames, const BusEffects& fx) {
        low_pass = glide(low_pass, fx.low_pass > 0 ? fx.low_pass : 20000.0f, frames);
        high_pass = glide(high_pass, fx.high_pass > 0 ? fx.high_pass : 10.0f, frames);
        if (low_pass < 19000) {
            SvfCoefficients c(low_pass);
            for (u32 i = 0; i < frames; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    f32 low, band;
                    lp[ch].tick(buf[i * 2 + ch], c.a1, c.a2, c.a3, low, band);
                    buf[i * 2 + ch] = low;
                }
        }
        if (high_pass > 15) {
            SvfCoefficients c(high_pass);
            for (u32 i = 0; i < frames; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    f32 x = buf[i * 2 + ch], low, band;
                    hp[ch].tick(x, c.a1, c.a2, c.a3, low, band);
                    buf[i * 2 + ch] = x - SvfCoefficients::k * band - low;
                }
        }
        f32 target = std::clamp(fx.reverb, 0.0f, 1.0f) * 3.0f;
        if (target <= 0 && wet <= 0) return;
        if (!reverb_ready || wet <= 0) {
            prepare_reverb(); // starts from silence, not from an old tail
            wet = 0;
        }
        f32 feedback = std::clamp(fx.room_size, 0.0f, 1.0f) * 0.28f + 0.7f;
        f32 damp = std::clamp(fx.damping, 0.0f, 1.0f) * 0.4f;
        f32 step = (target - wet) / static_cast<f32>(frames);
        for (u32 i = 0; i < frames; ++i) {
            f32 in = (buf[i * 2] + buf[i * 2 + 1]) * 0.015f;
            f32 out[2] = {0, 0};
            for (int side = 0; side < 2; ++side) {
                for (Comb& c : combs[side]) out[side] += c.tick(in, feedback, damp);
                for (Allpass& a : allpasses[side]) out[side] = a.tick(out[side]);
            }
            wet += step;
            buf[i * 2] += out[0] * wet;
            buf[i * 2 + 1] += out[1] * wet;
        }
        wet = target;
    }
};

// Music streams

// A playing music track: decoder -> SDL resampler (to 48 kHz stereo) -> gain ramp -> its bus.
struct AudioEngine::Stream {
    std::shared_ptr<const std::vector<u8>> data;
    std::unique_ptr<detail::Decoder> decoder;
    SDL_AudioStream* resampler = nullptr;
    u32 channels = 2;
    f32 gain = 0, target = 1;
    f32 ramp = 0; // gain change per output frame; 0 = jump
    bool loop = true, ended = false, stopping = false;
    Bus bus = Bus::Music;

    ~Stream() {
        if (resampler) SDL_DestroyAudioStream(resampler);
    }
};

static f32 ramp_per_frame(f32 from, f32 to, f32 seconds) {
    return seconds > 0 ? std::abs(to - from) / (seconds * static_cast<f32>(Sound::sample_rate)) : 0.0f;
}

void AudioEngine::play_music(const Music& music, const MusicParams& params) {
    auto s = std::make_unique<Stream>();
    s->data = music.data();
    s->decoder = music.codec() == Music::Codec::Opus ? detail::open_opus(*s->data) : detail::open_vorbis(*s->data);
    if (!s->decoder) {
        error("audio: music can't be decoded");
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

// Called with mutex_ held. Adds each stream into its bus buffer.
void AudioEngine::mix_streams(u32 frames) {
    const int want = static_cast<int>(frames * Sound::channels * sizeof(f32));
    stream_buffer_.resize(frames * Sound::channels);
    std::vector<f32> decoded;
    for (auto& sp : streams_) {
        Stream& s = *sp;
        // Decode until the resampler holds enough output for this callback.
        int empty_reads = 0;
        while (!s.ended && SDL_GetAudioStreamAvailable(s.resampler) < want) {
            decoded.resize(2048 * s.channels);
            u32 got = s.decoder->read(decoded.data(), 2048);
            if (got > 0) {
                SDL_PutAudioStreamData(s.resampler, decoded.data(), static_cast<int>(got * s.channels * sizeof(f32)));
                empty_reads = 0;
            } else if (s.loop && ++empty_reads < 2) {
                s.decoder->rewind(); // gapless: the resampler keeps its history
            } else {
                SDL_FlushAudioStream(s.resampler);
                s.ended = true;
            }
        }
        int bytes = SDL_GetAudioStreamData(s.resampler, stream_buffer_.data(), want);
        u32 got_frames = bytes > 0 ? static_cast<u32>(bytes) / (Sound::channels * sizeof(f32)) : 0;
        f32* out = bus_buffers_[static_cast<usize>(s.bus)].data();
        for (u32 i = 0; i < got_frames; ++i) {
            if (s.ramp > 0) {
                if (s.gain < s.target) s.gain = std::min(s.target, s.gain + s.ramp);
                else if (s.gain > s.target) s.gain = std::max(s.target, s.gain - s.ramp);
            } else {
                s.gain = s.target;
            }
            out[i * 2] += stream_buffer_[i * 2] * s.gain;
            out[i * 2 + 1] += stream_buffer_[i * 2 + 1] * s.gain;
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

// AudioEngine

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

Voice AudioEngine::start(const Sound& sound, const PlayParams& params) {
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
    v = {};
    v.samples = sound.samples();
    v.params = params;
    v.generation = next_generation_++;
    if (next_generation_ == 0) next_generation_ = 1;
    v.active = true;
    return {slot, v.generation};
}

Voice AudioEngine::play(const Sound& sound, const PlayParams& params) {
    std::lock_guard lock(mutex_);
    return start(sound, params);
}

Voice AudioEngine::play_at(const Sound& sound, Vec2 position, const PlayParams& params, const Attenuation& attenuation) {
    std::lock_guard lock(mutex_);
    Voice voice = start(sound, params);
    if (VoiceState* v = find(voice)) {
        v->spatial = true;
        v->world_position = position;
        v->attenuation = attenuation;
    }
    return voice;
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

void AudioEngine::set_filters(Voice voice, f32 low_pass, f32 high_pass) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) {
        v->params.low_pass = std::max(low_pass, 0.0f);
        v->params.high_pass = std::max(high_pass, 0.0f);
    }
}

void AudioEngine::set_ducking(Bus bus, std::optional<Ducking> ducking) {
    std::lock_guard lock(mutex_);
    ducking_[static_cast<usize>(bus)] = ducking;
    if (!ducking) duck_[static_cast<usize>(bus)] = 1;
}

f32 AudioEngine::duck_level(Bus bus) const {
    std::lock_guard lock(mutex_);
    return duck_[static_cast<usize>(bus)];
}

void AudioEngine::set_position(Voice voice, Vec2 position) {
    std::lock_guard lock(mutex_);
    if (VoiceState* v = find(voice)) v->world_position = position;
}

void AudioEngine::set_listener(Vec2 position) {
    std::lock_guard lock(mutex_);
    listener_ = position;
}

Vec2 AudioEngine::listener() const {
    std::lock_guard lock(mutex_);
    return listener_;
}

void AudioEngine::set_bus_gain(Bus bus, f32 gain) {
    std::lock_guard lock(mutex_);
    bus_gain_[static_cast<usize>(bus)] = std::max(gain, 0.0f);
}

f32 AudioEngine::bus_gain(Bus bus) const {
    std::lock_guard lock(mutex_);
    return bus_gain_[static_cast<usize>(bus)];
}

void AudioEngine::set_bus_effects(Bus bus, const BusEffects& effects) {
    std::lock_guard lock(mutex_);
    bus_effects_[static_cast<usize>(bus)] = effects;
    if (!effects_[static_cast<usize>(bus)]) effects_[static_cast<usize>(bus)] = std::make_unique<Effects>();
}

BusEffects AudioEngine::bus_effects(Bus bus) const {
    std::lock_guard lock(mutex_);
    return bus_effects_[static_cast<usize>(bus)];
}

u32 AudioEngine::active_voices() const {
    std::lock_guard lock(mutex_);
    return static_cast<u32>(std::count_if(voices_.begin(), voices_.end(), [](const VoiceState& v) { return v.active; }));
}

void AudioEngine::mix(f32* out, u32 frames) {
    std::lock_guard lock(mutex_);
    for (auto& b : bus_buffers_) b.assign(frames * Sound::channels, 0.0f);

    // Voices into their buses.
    for (VoiceState& v : voices_) {
        if (!v.active) continue;
        const std::vector<f32>& s = *v.samples;
        const f64 length = static_cast<f64>(s.size() / 2);
        if (length < 1) { v = {}; continue; }
        f32 gain = v.params.gain, pan = v.params.pan;
        if (v.spatial) {
            Spatial sp = spatialize(v.world_position, listener_, v.attenuation);
            gain *= sp.gain;
            pan = sp.pan;
        }
        f32 angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * (pi / 4.0f);
        f32 gl = gain * std::cos(angle) * std::numbers::sqrt2_v<f32>;
        f32 gr = gain * std::sin(angle) * std::numbers::sqrt2_v<f32>;
        f32* bus = bus_buffers_[static_cast<usize>(v.params.bus)].data();
        f64 step = v.params.pitch;
        // The voice's own filters, gliding like the buses' do.
        v.low_pass_now = Effects::glide(v.low_pass_now, v.params.low_pass > 0 ? v.params.low_pass : 20000.0f, frames);
        v.high_pass_now = Effects::glide(v.high_pass_now, v.params.high_pass > 0 ? v.params.high_pass : 10.0f, frames);
        bool lp = v.low_pass_now < 19000, hp = v.high_pass_now > 15;
        SvfCoefficients lpc(v.low_pass_now), hpc(v.high_pass_now);
        auto filter = [&](f32 x, int ch) {
            f32 low, band;
            if (lp) {
                Svf f{v.filter_state[ch * 2], v.filter_state[ch * 2 + 1]};
                f.tick(x, lpc.a1, lpc.a2, lpc.a3, low, band);
                v.filter_state[ch * 2] = f.ic1;
                v.filter_state[ch * 2 + 1] = f.ic2;
                x = low;
            }
            if (hp) {
                Svf f{v.filter_state[4 + ch * 2], v.filter_state[5 + ch * 2]};
                f.tick(x, hpc.a1, hpc.a2, hpc.a3, low, band);
                v.filter_state[4 + ch * 2] = f.ic1;
                v.filter_state[5 + ch * 2] = f.ic2;
                x = x - SvfCoefficients::k * band - low;
            }
            return x;
        };
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
            if (lp || hp) {
                l = filter(l, 0);
                r = filter(r, 1);
            }
            bus[i * 2] += l * gl;
            bus[i * 2 + 1] += r * gr;
            v.position += step;
        }
    }
    mix_streams(frames);

    // Ducking: a bus dips while its trigger bus is playing, judged by the trigger's loudest sample this block.
    std::array<f32, bus_count> duck_from = duck_;
    for (usize b = 0; b < bus_count; ++b) {
        if (!ducking_[b]) continue;
        const Ducking& d = *ducking_[b];
        f32 peak = 0;
        for (f32 x : bus_buffers_[static_cast<usize>(d.trigger)]) peak = std::max(peak, std::abs(x));
        bool loud = peak > d.threshold;
        f32 target = loud ? 1.0f - std::clamp(d.depth, 0.0f, 1.0f) : 1.0f;
        f32 seconds = std::max(loud ? d.attack : d.release, 1e-3f);
        f32 k = 1.0f - std::exp(-static_cast<f32>(frames) / (seconds * Sound::sample_rate));
        duck_[b] += (target - duck_[b]) * k;
    }

    // Each bus through its effects and gain into the master bus, then the master's own.
    constexpr usize master = static_cast<usize>(Bus::Master);
    std::vector<f32>& sum = bus_buffers_[master];
    for (usize b = 0; b < bus_count; ++b) {
        if (b == master) continue;
        if (effects_[b]) effects_[b]->process(bus_buffers_[b].data(), frames, bus_effects_[b]);
        f32 g = bus_gain_[b];
        if (duck_from[b] == 1.0f && duck_[b] == 1.0f) {
            for (u32 i = 0; i < frames * 2; ++i) sum[i] += bus_buffers_[b][i] * g;
        } else { // ramped across the block, so the dip never clicks
            for (u32 i = 0; i < frames; ++i) {
                f32 dg = g * (duck_from[b] + (duck_[b] - duck_from[b]) * static_cast<f32>(i) / static_cast<f32>(frames));
                sum[i * 2] += bus_buffers_[b][i * 2] * dg;
                sum[i * 2 + 1] += bus_buffers_[b][i * 2 + 1] * dg;
            }
        }
    }
    if (effects_[master]) effects_[master]->process(sum.data(), frames, bus_effects_[master]);
    f32 g = bus_gain_[master];
    // Soft clip instead of wrapping when many loud voices stack up.
    for (u32 i = 0; i < frames * 2; ++i) {
        f32 x = sum[i] * g;
        out[i] = x > 1.0f || x < -1.0f ? std::tanh(x) : x;
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

bool is_ogg(std::string_view path) { return path.ends_with(".ogg") || path.ends_with(".oga") || path.ends_with(".opus"); }

// A whole sound file decoded to the mixer's format. Thread-safe: background loads run it on the loading thread.
std::unique_ptr<Sound> decode_sound(std::string_view path, std::span<const u8> bytes) {
    std::vector<u8> copy(bytes.begin(), bytes.end());
    if (detail::looks_like_opus(copy)) {
        auto decoder = detail::open_opus(copy);
        if (!decoder) {
            log_asset_error(path, "Sound", opus_supported() ? "not a valid Ogg Opus file" : "Opus needs the engine built with -DBOWL_OPUS=ON");
            return nullptr;
        }
        std::vector<f32> samples; // already 48 kHz stereo
        std::vector<f32> block(4096 * 2);
        while (u32 got = decoder->read(block.data(), 4096)) samples.insert(samples.end(), block.begin(), block.begin() + got * 2);
        return std::make_unique<Sound>(std::move(samples));
    }
    if (is_ogg(path)) {
        int channels = 0, rate = 0;
        short* pcm = nullptr;
        int frames = stb_vorbis_decode_memory(copy.data(), static_cast<int>(copy.size()), &channels, &rate, &pcm);
        if (frames <= 0 || !pcm) {
            log_asset_error(path, "Sound", "not a valid Ogg Vorbis file");
            return nullptr;
        }
        SDL_AudioSpec spec{SDL_AUDIO_S16, channels, rate};
        auto sound = to_mixer_format(path, spec, pcm, frames * channels * static_cast<int>(sizeof(short)));
        std::free(pcm);
        return sound;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(copy.data(), copy.size());
    SDL_AudioSpec spec;
    Uint8* data = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV_IO(io, true, &spec, &data, &length)) {
        log_asset_error(path, "Sound", SDL_GetError());
        return nullptr;
    }
    auto sound = to_mixer_format(path, spec, data, static_cast<int>(length));
    SDL_free(data);
    return sound;
}

struct DecodedSound {
    std::unique_ptr<Sound> sound; // null if decoding failed (and said why)
};

} // namespace

void register_asset_loaders(AssetManager& assets) {
    // Sound: decoded fully up front. Use for effects; use Music for long tracks.
    assets.register_loader<Sound>([](AssetLoadContext& ctx) -> std::unique_ptr<Sound> {
        if (auto* decoded = ctx.prepared<DecodedSound>()) return std::move(decoded->sound); // a background load
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Sound", "file not found in any mount");
            return nullptr;
        }
        return decode_sound(ctx.path(), *bytes);
    });
    assets.register_decoder<Sound>([](std::string_view path, std::span<const u8> bytes) -> std::shared_ptr<void> {
        return std::make_shared<DecodedSound>(DecodedSound{decode_sound(path, bytes)});
    });

    assets.register_loader<Music>([](AssetLoadContext& ctx) -> std::unique_ptr<Music> {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Music", "file not found in any mount");
            return nullptr;
        }
        bool opus = detail::looks_like_opus(*bytes);
        auto music = Music::from_memory(std::move(*bytes));
        if (!music)
            log_asset_error(ctx.path(), "Music",
                            !opus ? "not a valid mono/stereo Ogg Vorbis file"
                                  : (opus_supported() ? "not a valid Ogg Opus file"
                                                      : "Opus needs the engine built with -DBOWL_OPUS=ON"));
        return music;
    });
}

} // namespace bowl::audio
