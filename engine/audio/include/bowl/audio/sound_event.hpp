#pragma once

#include "bowl/audio/audio.hpp"
#include "bowl/core/function_ref.hpp"
#include "bowl/core/json.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bowl::audio {

// A sound the game asks for by what happened ("footstep", "door slam") rather than by file: which clip plays, how loud
// and at what pitch is picked each time, within limits the event's .sound file sets. Loaded as an asset from .sound
// JSON files, which hot-reload; play one with AudioEngine::play_event. See docs/guides/audio.md, "Sound events".
class SoundEvent {
public:
    // Picked anywhere between min and max, evenly.
    struct Range {
        f32 min = 1, max = 1;
    };
    struct Clip {
        std::string path; // as the file names it
        Asset<Sound> sound;
        f32 weight = 1; // how often it's picked compared with the others in its list
    };
    using ClipList = std::vector<Clip>;

    // Clips that play together with the other layers' (a footstep and the rustle of clothes). One clip of the layer's
    // list plays each time; with a switch, the list is the one for the switch's value in EventParams.
    struct Layer {
        ClipList clips;          // without a switch, or when its value isn't listed
        std::string switch_name; // e.g. "surface"; empty: no switch
        std::vector<std::pair<std::string, ClipList>> switch_values; // {"grass", [...]}, in file order
        Range gain{1, 1};
        Range pitch{1, 1};
        Range delay{0, 0};
    };

    std::string path; // the asset path it was loaded from; empty for an event made in code
    // Tells AudioEngine's instance limits which event this is when path is empty. Copies share it.
    u64 id = next_id();
    std::vector<Layer> layers;
    Range gain{1, 1};
    Range pitch{1, 1};
    Range delay{0, 0}; // seconds before it starts
    bool no_repeat = true; // never the same clip twice in a row, when a list has more than one
    Bus bus = Bus::Effects;
    bool loop = false;
    f32 low_pass = 0, high_pass = 0; // Hz; 0 = off
    Attenuation attenuation;         // for events played at a position

    enum class Steal : u8 {
        Oldest,   // the one playing longest stops for the new one
        Quietest, // the quietest stops for the new one (counting distance, for events in the world)
        None,     // the new one doesn't play
    };
    u32 max_instances = 0; // how many may play at once; 0 = any number
    Steal steal = Steal::Oldest;
    f32 cooldown = 0; // seconds after it plays before it can play again

    // The list a layer picks from, given the switches: the switch value's list, else the layer's own clips, else the
    // first value's list. Null if there is nothing to pick from.
    const ClipList* clips_for(const Layer& layer, const EventParams& params) const;

    // Reads an event from its JSON. `load` turns each clip's path into a Sound (AssetManager::load<Sound>, say).
    // False, with `error` saying what's wrong, if the document isn't a valid event.
    static bool parse(const json::Value& doc, FunctionRef<Asset<Sound>(std::string_view)> load, SoundEvent& out,
                      std::string& error);

private:
    static u64 next_id();
};

// Registers the SoundEvent loader (.sound files) with the asset manager; register_asset_loaders calls it.
void register_sound_event_loader(AssetManager& assets);

} // namespace bowl::audio
