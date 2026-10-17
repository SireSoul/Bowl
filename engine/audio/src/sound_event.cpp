#include "bowl/audio/sound_event.hpp"

#include "bowl/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>

namespace bowl::audio {

// Reading .sound files

namespace {

struct Reader {
    FunctionRef<Asset<Sound>(std::string_view)> load;
    std::string& error;

    bool fail(std::string message) {
        error = std::move(message);
        return false;
    }

    bool keys(const json::Value& object, std::string_view where, std::initializer_list<std::string_view> allowed) {
        for (const auto& [key, value] : object.object())
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                return fail(std::format("{}: unknown key \"{}\"", where, key));
        return true;
    }

    bool number(const json::Value& v, std::string_view name, f32 lo, f32 hi, f32& out) {
        if (!v.is_number()) return fail(std::format("\"{}\" must be a number", name));
        f64 n = v.as_number();
        if (!std::isfinite(n) || n < lo || n > hi) return fail(std::format("\"{}\" must be between {} and {}", name, lo, hi));
        out = static_cast<f32>(n);
        return true;
    }

    // A number, or [min, max].
    bool range(const json::Value& v, std::string_view name, f32 lo, f32 hi, SoundEvent::Range& out) {
        if (v.is_number()) {
            if (!number(v, name, lo, hi, out.min)) return false;
            out.max = out.min;
            return true;
        }
        if (!v.is_array() || v.size() != 2) return fail(std::format("\"{}\" must be a number or [min, max]", name));
        if (!number(v[0], name, lo, hi, out.min) || !number(v[1], name, lo, hi, out.max)) return false;
        if (out.min > out.max) return fail(std::format("\"{}\": min is more than max", name));
        return true;
    }

    bool clip_list(const json::Value& v, std::string_view where, std::string_view dir, SoundEvent::ClipList& out) {
        if (!v.is_array()) return fail(std::format("{} must be a list of clips", where));
        for (const json::Value& c : v.array()) {
            SoundEvent::Clip clip;
            std::string_view file;
            if (c.is_string()) {
                file = c.as_string();
            } else if (c.is_object()) {
                if (!keys(c, where, {"sound", "weight"})) return false;
                file = c["sound"].as_string();
                if (const json::Value* w = c.find("weight"); w && !number(*w, "weight", 0, 1e6f, clip.weight)) return false;
            } else {
                return fail(std::format("{}: a clip is a path or {{\"sound\": path, \"weight\": n}}", where));
            }
            if (file.empty()) return fail(std::format("{}: a clip has no sound path", where));
            // Relative to the .sound file's folder; a leading / starts from the top of the assets.
            clip.path = file.starts_with('/') ? std::string(file.substr(1)) : std::string(dir) + std::string(file);
            clip.sound = load(clip.path);
            out.push_back(std::move(clip));
        }
        return true;
    }

    // "switch": {"surface": {"grass": [...], "stone": [...]}}
    bool switch_(const json::Value& v, std::string_view dir, SoundEvent::Layer& layer) {
        if (!v.is_object() || v.size() != 1 || !v.object()[0].second.is_object())
            return fail("\"switch\" must be {\"name\": {\"value\": [clips], ...}}");
        const auto& [name, values] = v.object()[0];
        if (name.empty()) return fail("\"switch\" needs a name");
        layer.switch_name = name;
        for (const auto& [value, clips] : values.object()) {
            layer.switch_values.emplace_back(value, SoundEvent::ClipList{});
            if (!clip_list(clips, std::format("switch \"{}\": \"{}\"", name, value), dir, layer.switch_values.back().second))
                return false;
        }
        return true;
    }

    bool layer(const json::Value& v, std::string_view dir, SoundEvent::Layer& out) {
        if (!v.is_object()) return fail("each of \"layers\" must be an object");
        if (!keys(v, "layer", {"clips", "switch", "gain", "pitch", "delay"})) return false;
        if (const json::Value* c = v.find("clips"); c && !clip_list(*c, "\"clips\"", dir, out.clips)) return false;
        if (const json::Value* s = v.find("switch"); s && !switch_(*s, dir, out)) return false;
        if (const json::Value* g = v.find("gain"); g && !range(*g, "gain", 0, 16, out.gain)) return false;
        if (const json::Value* p = v.find("pitch"); p && !range(*p, "pitch", 0.01f, 16, out.pitch)) return false;
        if (const json::Value* d = v.find("delay"); d && !range(*d, "delay", 0, 60, out.delay)) return false;
        return true;
    }
};

bool has_clips(const SoundEvent::Layer& layer) {
    if (!layer.clips.empty()) return true;
    return std::any_of(layer.switch_values.begin(), layer.switch_values.end(), [](const auto& v) { return !v.second.empty(); });
}

constexpr std::pair<std::string_view, Bus> bus_names[] = {{"master", Bus::Master},     {"music", Bus::Music},
                                                          {"effects", Bus::Effects},   {"ambience", Bus::Ambience},
                                                          {"voice", Bus::Voice},       {"ui", Bus::Ui}};
constexpr std::pair<std::string_view, SoundEvent::Steal> steal_names[] = {
    {"oldest", SoundEvent::Steal::Oldest}, {"quietest", SoundEvent::Steal::Quietest}, {"none", SoundEvent::Steal::None}};

template <class T, usize N>
bool enum_value(Reader& r, const json::Value& v, std::string_view name, const std::pair<std::string_view, T> (&names)[N], T& out) {
    for (const auto& [text, value] : names)
        if (v.as_string() == text) {
            out = value;
            return true;
        }
    std::string choices;
    for (const auto& [text, value] : names) choices += std::format("{}\"{}\"", choices.empty() ? "" : ", ", text);
    return r.fail(std::format("\"{}\" must be one of {}", name, choices));
}

} // namespace

u64 SoundEvent::next_id() {
    static std::atomic<u64> next{1};
    return next++;
}

bool SoundEvent::parse(const json::Value& doc, FunctionRef<Asset<Sound>(std::string_view)> load, SoundEvent& out,
                       std::string& error) {
    Reader r{load, error};
    if (!doc.is_object()) return r.fail("a sound event is a JSON object");
    if (!r.keys(doc, "sound event",
                {"clips", "switch", "layers", "gain", "pitch", "delay", "no_repeat", "bus", "loop", "low_pass", "high_pass",
                 "attenuation", "max_instances", "steal", "cooldown"}))
        return false;
    std::string_view dir = out.path;
    dir = dir.substr(0, dir.rfind('/') == std::string_view::npos ? 0 : dir.rfind('/') + 1);

    // Clips and a switch at the top are its first layer.
    out.layers.clear();
    if (doc.contains("clips") || doc.contains("switch")) {
        SoundEvent::Layer first;
        if (const json::Value* c = doc.find("clips"); c && !r.clip_list(*c, "\"clips\"", dir, first.clips)) return false;
        if (const json::Value* s = doc.find("switch"); s && !r.switch_(*s, dir, first)) return false;
        out.layers.push_back(std::move(first));
    }
    if (const json::Value* layers = doc.find("layers")) {
        if (!layers->is_array()) return r.fail("\"layers\" must be a list");
        for (const json::Value& l : layers->array()) {
            out.layers.emplace_back();
            if (!r.layer(l, dir, out.layers.back())) return false;
        }
    }
    if (std::none_of(out.layers.begin(), out.layers.end(), has_clips))
        return r.fail("no clips: give \"clips\", a \"switch\" or \"layers\"");

    if (const json::Value* v = doc.find("gain"); v && !r.range(*v, "gain", 0, 16, out.gain)) return false;
    if (const json::Value* v = doc.find("pitch"); v && !r.range(*v, "pitch", 0.01f, 16, out.pitch)) return false;
    if (const json::Value* v = doc.find("delay"); v && !r.range(*v, "delay", 0, 60, out.delay)) return false;
    if (const json::Value* v = doc.find("low_pass"); v && !r.number(*v, "low_pass", 0, 24000, out.low_pass)) return false;
    if (const json::Value* v = doc.find("high_pass"); v && !r.number(*v, "high_pass", 0, 24000, out.high_pass)) return false;
    if (const json::Value* v = doc.find("cooldown"); v && !r.number(*v, "cooldown", 0, 3600, out.cooldown)) return false;
    for (std::string_view flag : {"no_repeat", "loop"})
        if (const json::Value* v = doc.find(flag)) {
            if (!v->is_bool()) return r.fail(std::format("\"{}\" must be true or false", flag));
            (flag == "loop" ? out.loop : out.no_repeat) = v->as_bool();
        }
    if (const json::Value* v = doc.find("bus"); v && !enum_value(r, *v, "bus", bus_names, out.bus)) return false;
    if (const json::Value* v = doc.find("steal"); v && !enum_value(r, *v, "steal", steal_names, out.steal)) return false;
    if (const json::Value* v = doc.find("max_instances")) {
        f32 n = 0;
        if (!r.number(*v, "max_instances", 0, AudioEngine::max_voices, n)) return false;
        if (n != std::floor(n)) return r.fail("\"max_instances\" must be a whole number");
        out.max_instances = static_cast<u32>(n);
    }
    if (const json::Value* v = doc.find("attenuation")) {
        if (!v->is_object()) return r.fail("\"attenuation\" must be {\"near\": n, \"far\": n, \"pan_distance\": n}");
        if (!r.keys(*v, "attenuation", {"near", "far", "pan_distance"})) return false;
        Attenuation& a = out.attenuation;
        if (const json::Value* n = v->find("near"); n && !r.number(*n, "near", 0, 1e7f, a.near)) return false;
        if (const json::Value* n = v->find("far"); n && !r.number(*n, "far", 0, 1e7f, a.far)) return false;
        if (const json::Value* n = v->find("pan_distance"); n && !r.number(*n, "pan_distance", 0, 1e7f, a.pan_distance))
            return false;
        if (a.far < a.near) return r.fail("attenuation: \"far\" is nearer than \"near\"");
    }
    return true;
}

const SoundEvent::ClipList* SoundEvent::clips_for(const Layer& layer, const EventParams& params) const {
    if (!layer.switch_name.empty()) {
        std::string_view value = params.switch_value(layer.switch_name);
        for (const auto& [name, clips] : layer.switch_values)
            if (name == value) return &clips;
    }
    if (!layer.clips.empty()) return &layer.clips;
    if (!layer.switch_values.empty()) return &layer.switch_values.front().second;
    return nullptr;
}

std::string_view EventParams::switch_value(std::string_view name) const {
    for (const auto& [key, value] : switches)
        if (key == name) return value;
    return {};
}

void register_sound_event_loader(AssetManager& assets) {
    assets.register_loader<SoundEvent>([](AssetLoadContext& ctx) -> std::unique_ptr<SoundEvent> {
        auto text = ctx.read_text(ctx.path());
        if (!text) {
            log_asset_error(ctx.path(), "SoundEvent", "file not found in any mount");
            return nullptr;
        }
        auto parsed = json::parse(*text);
        if (!parsed) {
            log_asset_error(ctx.path(), "SoundEvent",
                            std::format("JSON error at {}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message));
            return nullptr;
        }
        auto event = std::make_unique<SoundEvent>();
        event->path = ctx.path();
        std::string error;
        auto load = [&](std::string_view clip) { return ctx.assets().load<Sound>(clip); };
        if (!SoundEvent::parse(parsed.value, load, *event, error)) {
            log_asset_error(ctx.path(), "SoundEvent", error);
            return nullptr;
        }
        return event;
    });
}

// Playing them. Everything here runs on the calling thread under mutex_, built from start(); the audio thread only
// ever sees ordinary voices.

namespace {

std::string event_key(const SoundEvent& event) {
    return event.path.empty() ? std::format("#{}", event.id) : event.path;
}

f32 pick(SoundEvent::Range r, f64 random) { return r.min + (r.max - r.min) * static_cast<f32>(random); }

} // namespace

f64 AudioEngine::random() {
    // splitmix64
    u64 z = (random_state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<f64>(z >> 11) * 0x1.0p-53;
}

void AudioEngine::seed_random(u64 seed) {
    std::lock_guard lock(mutex_);
    random_state_ = seed;
}

Voice AudioEngine::start_event_voice(const EventVoice& voice, const EventLive& live) {
    Voice v = start(voice.sound, voice.params);
    if (VoiceState* s = find(v); s && live.spatial) {
        s->spatial = true;
        s->world_position = live.position;
        s->attenuation = live.attenuation;
    }
    return v;
}

void AudioEngine::stop_live(EventLive& live) {
    for (Voice v : live.voices)
        if (VoiceState* s = find(v)) *s = {};
    live.voices.clear();
    live.waiting.clear();
}

bool AudioEngine::live_alive(const EventLive& live) const {
    return !live.waiting.empty() || std::any_of(live.voices.begin(), live.voices.end(), [&](Voice v) { return find(v); });
}

EventInstance AudioEngine::play_event(const Asset<SoundEvent>& event, const EventParams& params) {
    return event ? play_event(*event, params) : EventInstance{};
}

EventInstance AudioEngine::play_event(const SoundEvent& event, const EventParams& params) {
    std::lock_guard lock(mutex_);
    std::string key = event_key(event);
    EventState& state = events_[key];
    std::erase_if(live_events_, [&](const EventLive& l) { return !live_alive(l); });
    if (event.cooldown > 0 && clock_ - state.last_played < event.cooldown) return {};

    usize playing = 0;
    for (const EventLive& l : live_events_) playing += l.event == key;
    bool full = event.max_instances > 0 && playing >= event.max_instances;
    if (full && event.steal == SoundEvent::Steal::None) return {};

    // Pick what plays: the event's gain, pitch and delay, then a clip and the layer's own for each layer.
    EventLive live;
    live.event = key;
    live.spatial = params.position.has_value();
    live.position = params.position.value_or(Vec2{});
    live.attenuation = event.attenuation;
    live.started = clock_;
    f32 gain = pick(event.gain, random()) * std::max(params.gain, 0.0f);
    f32 pitch = pick(event.pitch, random());
    f32 delay = pick(event.delay, random());
    live.gain = gain;
    std::vector<EventPick> picks;
    for (usize li = 0; li < event.layers.size(); ++li) {
        const SoundEvent::Layer& layer = event.layers[li];
        const SoundEvent::ClipList* clips = event.clips_for(layer, params);
        if (!clips) continue;
        // Each list remembers its own last pick: 0 is the layer's clips, 1 + i the i-th switch value's.
        u64 list = 0;
        for (usize i = 0; i < layer.switch_values.size(); ++i)
            if (clips == &layer.switch_values[i].second) list = 1 + i;
        u64 memory = (static_cast<u64>(li) << 32) | list;
        auto last = std::find_if(state.last_clip.begin(), state.last_clip.end(), [&](const auto& m) { return m.first == memory; });
        u32 skip = last != state.last_clip.end() && event.no_repeat ? last->second : ~0u;

        // Weighted, among the clips that loaded, leaving out the last one picked when there's another to pick.
        auto usable = [&](usize i) { return (*clips)[i].sound && (*clips)[i].weight > 0; };
        usize candidates = 0;
        for (usize i = 0; i < clips->size(); ++i) candidates += usable(i);
        if (candidates < 2) skip = ~0u;
        f64 total = 0;
        for (usize i = 0; i < clips->size(); ++i)
            if (usable(i) && i != skip) total += (*clips)[i].weight;
        f64 r = random() * total;
        usize chosen = clips->size();
        for (usize i = 0; i < clips->size() && total > 0; ++i) {
            if (!usable(i) || i == skip) continue;
            chosen = i;
            r -= (*clips)[i].weight;
            if (r < 0) break;
        }
        f32 layer_gain = pick(layer.gain, random());
        f32 layer_pitch = pick(layer.pitch, random());
        f32 layer_delay = pick(layer.delay, random());
        if (chosen == clips->size()) continue;
        if (last != state.last_clip.end()) last->second = static_cast<u32>(chosen);
        else state.last_clip.emplace_back(memory, static_cast<u32>(chosen));

        const SoundEvent::Clip& clip = (*clips)[chosen];
        EventVoice voice{*clip.sound,
                         {.gain = gain * layer_gain,
                          .pitch = std::max(pitch * layer_pitch, 0.01f),
                          .loop = event.loop,
                          .bus = event.bus,
                          .low_pass = event.low_pass,
                          .high_pass = event.high_pass},
                         clock_ + delay + layer_delay};
        picks.push_back({clip.path, voice.params.gain, voice.params.pitch, delay + layer_delay});
        live.waiting.push_back(std::move(voice));
    }
    if (live.waiting.empty()) return {};

    if (full) { // make room: the oldest, or the quietest where the listener is now
        auto victim = live_events_.end();
        f64 best = 0;
        for (auto it = live_events_.begin(); it != live_events_.end(); ++it) {
            if (it->event != key) continue;
            f64 score = it->started;
            if (event.steal == SoundEvent::Steal::Quietest)
                score = it->gain * (it->spatial ? spatialize(it->position, listener_, it->attenuation).gain : 1.0f);
            if (victim == live_events_.end() || score < best) {
                victim = it;
                best = score;
            }
        }
        if (victim != live_events_.end()) {
            stop_live(*victim);
            live_events_.erase(victim);
        }
    }

    live.id = next_instance_++;
    if (next_instance_ == 0) next_instance_ = 1;
    std::erase_if(live.waiting, [&](const EventVoice& v) {
        if (v.start_at > clock_) return false;
        live.voices.push_back(start_event_voice(v, live));
        return true;
    });
    state.last_played = clock_;
    state.last_picks = std::move(picks);
    EventInstance instance{live.id};
    live_events_.push_back(std::move(live));
    return instance;
}

void AudioEngine::update(f32 dt) {
    std::lock_guard lock(mutex_);
    clock_ += std::max(dt, 0.0f);
    for (EventLive& live : live_events_)
        std::erase_if(live.waiting, [&](const EventVoice& v) {
            if (v.start_at > clock_) return false;
            live.voices.push_back(start_event_voice(v, live));
            return true;
        });
    std::erase_if(live_events_, [&](const EventLive& l) { return !live_alive(l); });
}

void AudioEngine::stop(EventInstance instance) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(live_events_.begin(), live_events_.end(), [&](const EventLive& l) { return l.id == instance.id; });
    if (!instance || it == live_events_.end()) return;
    stop_live(*it);
    live_events_.erase(it);
}

bool AudioEngine::playing(EventInstance instance) const {
    std::lock_guard lock(mutex_);
    if (!instance) return false;
    auto it = std::find_if(live_events_.begin(), live_events_.end(), [&](const EventLive& l) { return l.id == instance.id; });
    return it != live_events_.end() && live_alive(*it);
}

void AudioEngine::set_position(EventInstance instance, Vec2 position) {
    std::lock_guard lock(mutex_);
    if (!instance) return;
    for (EventLive& live : live_events_) {
        if (live.id != instance.id) continue;
        live.position = position;
        for (Voice v : live.voices)
            if (VoiceState* s = find(v)) s->world_position = position;
    }
}

std::vector<Voice> AudioEngine::voices(EventInstance instance) const {
    std::lock_guard lock(mutex_);
    std::vector<Voice> out;
    if (!instance) return out;
    for (const EventLive& live : live_events_)
        if (live.id == instance.id)
            for (Voice v : live.voices)
                if (find(v)) out.push_back(v);
    return out;
}

std::vector<EventPick> AudioEngine::last_picks(const SoundEvent& event) const {
    std::lock_guard lock(mutex_);
    auto it = events_.find(event_key(event));
    return it != events_.end() ? it->second.last_picks : std::vector<EventPick>{};
}

} // namespace bowl::audio
