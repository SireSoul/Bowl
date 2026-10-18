// Sound events: .sound files (JSON), which a game reads from its assets and a modder may edit.
//
// Input: the .sound file. Every clip it names is the same short sound, so what parses also gets played: picked,
// limited, delayed and mixed offline.

#include "fuzz.hpp"

#include "bowl/audio/sound_event.hpp"

#include <memory>

using namespace bowl;
using namespace bowl::audio;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static AssetManager assets;
    static Asset<Sound> clip = assets.add("clip", std::make_unique<Sound>(std::vector<f32>(2 * 64, 0.25f)));
    auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!doc) return 0;
    SoundEvent event;
    event.path = "sfx/fuzz.sound";
    std::string error;
    auto load = [](std::string_view) { return clip; };
    if (!SoundEvent::parse(doc.value, load, event, error)) {
        BOWL_FUZZ_CHECK(!error.empty());
        return 0;
    }

    // What a game does with one: play it now and then, with and without switches, at a place, and let time pass.
    AudioEngine audio;
    audio.seed_random(size);
    std::vector<f32> out(2 * 256);
    std::vector<EventInstance> played;
    for (int i = 0; i < 12; ++i) {
        EventParams params;
        if (i % 3 == 1) params.position = Vec2{static_cast<f32>(i * 50), 0};
        for (const SoundEvent::Layer& layer : event.layers)
            if (!layer.switch_name.empty() && !layer.switch_values.empty())
                params.switches.emplace_back(layer.switch_name, layer.switch_values[static_cast<usize>(i) % layer.switch_values.size()].first);
        played.push_back(audio.play_event(event, params));
        for (const EventPick& p : audio.last_picks(event)) BOWL_FUZZ_CHECK(p.gain >= 0 && p.pitch > 0 && p.delay >= 0);
        audio.update(0.05f);
        audio.mix(out.data(), 256);
        BOWL_FUZZ_CHECK(audio.active_voices() <= AudioEngine::max_voices);
    }
    for (EventInstance instance : played) {
        audio.set_position(instance, {1, 2});
        audio.voices(instance);
        audio.stop(instance);
        BOWL_FUZZ_CHECK(!audio.playing(instance));
    }
    return 0;
}
