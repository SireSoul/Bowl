# audio

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/audio/audio.hpp](#bowlaudioaudiohpp)

## bowl/audio/audio.hpp

`#include "bowl/audio/audio.hpp"` · namespace `bowl::audio`

### Sound

Decoded PCM: interleaved stereo float at the mixer rate. Shared, so a voice keeps playing the old data even if the
asset is hot-reloaded underneath it.

```cpp
class Sound
```

```cpp
static constexpr u32 sample_rate = 48000;
static constexpr u32 channels = 2;
```

```cpp
explicit Sound(std::vector<f32> interleaved_stereo);
```

Mono samples at sample_rate, e.g. synthesized effects.

```cpp
static std::unique_ptr<Sound> from_mono(std::span<const f32> mono);
```

```cpp
u32 frames() const;
f32 seconds() const;
const std::shared_ptr<const std::vector<f32>>& samples() const;
```

### Music

Compressed music (Ogg Vorbis, or Ogg Opus when built with BOWL_OPUS), decoded a little at a time while it plays.
Loaded as an asset from .ogg and .opus files.

```cpp
class Music
```

```cpp
enum class Codec : u8 { Vorbis, Opus };
```

```cpp
Music(std::shared_ptr<const std::vector<u8>> data, Codec codec, u32 sample_rate, u32 channels, f32 seconds);
```

Validates and measures an Ogg Vorbis or Opus file in memory; null if it isn't one (or Opus isn't built in).

```cpp
static std::unique_ptr<Music> from_memory(std::vector<u8> ogg);
```

```cpp
const std::shared_ptr<const std::vector<u8>>& data() const;
Codec codec() const;
u32 sample_rate() const;
u32 channels() const;
f32 seconds() const;
```

Whether .opus files can be loaded (the engine was built with -DBOWL_OPUS=ON).

```cpp
bool opus_supported();
```

```cpp
enum class Bus : u8 { Master, Music, Effects, Ambience, Voice, Ui };
inline constexpr usize bus_count = 6;
```

### Attenuation

How loud a sound in the world is from where the listener is. Full volume within `near`, silent beyond `far`,
fading smoothly between; panned fully to one side once it's `pan_distance` to the left or right.

```cpp
struct Attenuation {
    f32 near = 64;
    f32 far = 1024;
    f32 pan_distance = 512;
};
```

### PlayParams

```cpp
struct PlayParams {
    f32 gain = 1;
    f32 pan = 0;   // -1 left .. +1 right (equal-power)
    f32 pitch = 1; // playback rate
    bool loop = false;
    Bus bus = Bus::Effects;
    // Filters on this sound alone (a voice through a door, a muffled explosion far away). Hz; 0 = off.
    f32 low_pass = 0;
    f32 high_pass = 0;
};
```

### Ducking

One bus getting quieter while another plays: music dipping under dialogue. `depth` is how much quieter (0..1);
it dips over `attack` seconds once the trigger bus is louder than `threshold`, and comes back over `release`.

```cpp
struct Ducking {
    Bus trigger = Bus::Voice;
    f32 depth = 0.5f;
    f32 attack = 0.08f;
    f32 release = 0.5f;
    f32 threshold = 0.02f;
};
```

### MusicParams

```cpp
struct MusicParams {
    f32 gain = 1;
    f32 fade_in = 0;   // seconds; the previous track fades out over the same time (crossfade)
    bool loop = true;  // gapless
    Bus bus = Bus::Music;
};
```

### BusEffects

Effects on everything that plays through a bus, applied before the bus gain. Changes are smoothed, so they can
follow gameplay (muffle the music while paused, add reverb in a cave).

```cpp
struct BusEffects {
    f32 low_pass = 0;     // Hz: cut what's above (muffled, underwater: 400-1000). 0 = off
    f32 high_pass = 0;    // Hz: cut what's below (thin, a radio: 1000-2000). 0 = off
    f32 reverb = 0;       // 0..1, how much of the reverb is heard
    f32 room_size = 0.5f; // 0..1, how long the reverb rings
    f32 damping = 0.5f;   // 0..1, how quickly its highs die away
};
```

### Spatial

The gain and pan for a sound at `source` heard from `listener`, both in world units.

```cpp
struct Spatial {
    f32 gain = 1;
    f32 pan = 0;
};
```

```cpp
Spatial spatialize(Vec2 source, Vec2 listener, const Attenuation& attenuation = {});
```

### Voice

Handle to a playing sound. Stale handles (sound already finished) are ignored safely.

```cpp
struct Voice
```

```cpp
u32 slot = 0;
u32 generation = 0;
explicit operator bool() const;
```

### AudioEngine

Software mixer on SDL's audio device. Every method is thread-safe; mixing happens on SDL's audio thread.

```cpp
class AudioEngine : NonCopyable
```

```cpp
static constexpr u32 max_voices = 128;
```

```cpp
AudioEngine();
~AudioEngine();
```

```cpp
bool init();
void shutdown();
bool active() const;
```

```cpp
Voice play(const Sound& sound, const PlayParams& params = {});
Voice play(const Asset<Sound>& sound, const PlayParams& params = {});
```

A sound at a place in the world: its gain and pan follow the listener as either moves. params.pan is ignored.

```cpp
Voice play_at(const Sound& sound, Vec2 position, const PlayParams& params = {}, const Attenuation& attenuation = {});
Voice play_at(const Asset<Sound>& sound, Vec2 position, const PlayParams& params = {},
              const Attenuation& attenuation = {});
void stop(Voice voice);
void stop_all();
bool playing(Voice voice) const;
void set_gain(Voice voice, f32 gain);
void set_pan(Voice voice, f32 pan);
void set_pitch(Voice voice, f32 pitch);
```

Changes a playing sound's filters (Hz, 0 = off); they glide there, so following gameplay every frame is fine.

```cpp
void set_filters(Voice voice, f32 low_pass, f32 high_pass = 0);
```

Moves a sound started with play_at.

```cpp
void set_position(Voice voice, Vec2 position);
```

Where the player hears from, usually the camera or the player character. Set it every frame.

```cpp
void set_listener(Vec2 position);
Vec2 listener() const;
```

Streams a music track, replacing (crossfading from) whatever music is playing.

```cpp
void play_music(const Music& music, const MusicParams& params = {});
void play_music(const Asset<Music>& music, const MusicParams& params = {});
void stop_music(f32 fade_out = 0);
bool music_playing() const;
```

```cpp
void set_bus_gain(Bus bus, f32 gain);
f32 bus_gain(Bus bus) const;
void set_bus_effects(Bus bus, const BusEffects& effects);
BusEffects bus_effects(Bus bus) const;
```

Makes `bus` dip while another bus plays (see Ducking); std::nullopt turns it off.

```cpp
void set_ducking(Bus bus, std::optional<Ducking> ducking);
```

How far a ducked bus is dipped right now: 1 is full volume.

```cpp
f32 duck_level(Bus bus) const;
u32 active_voices() const;
```

Renders `frames` stereo frames into out (used by the device callback; public for offline rendering/tests).

```cpp
void mix(f32* out, u32 frames);
```

Registers the Sound (WAV, OGG, Opus) and Music (OGG, Opus; streamed) loaders with the asset manager.

```cpp
void register_asset_loaders(AssetManager& assets);
```

