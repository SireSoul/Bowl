# Audio

The engine mixes audio itself, on SDL's audio thread: up to 128 sounds at once, streamed music with crossfades, and
six volume buses. Every `AudioEngine` call is thread-safe.

## Sounds

Short effects are `audio::Sound` assets, decoded fully into memory when loaded:

```cpp
Asset<audio::Sound> jump = engine.assets().load<audio::Sound>("sfx/jump.wav");   // or .ogg

engine.audio().play(jump);
engine.audio().play(jump, {.gain = 0.6f, .pan = -0.5f, .pitch = 1.2f});
```

| `PlayParams` | Default | |
| --- | --- | --- |
| `gain` | 1 | Volume multiplier |
| `pan` | 0 | -1 is fully left, +1 fully right (equal-power, so the center isn't quieter) |
| `pitch` | 1 | Playback rate: 2 is an octave up and twice as fast |
| `loop` | `false` | Repeat until stopped |
| `bus` | `Effects` | Which volume bus it plays on |

WAV files (anything SDL reads: PCM, float, ADPCM) and Ogg Vorbis files are supported, mono or stereo, at any sample
rate; everything is converted to the mixer's 48 kHz stereo on load.

A small random pitch variation (`0.9` to `1.1`) keeps repeated sounds, like footsteps, from sounding mechanical.

### Controlling a playing sound

`play` returns a `Voice` handle:

```cpp
audio::Voice engine_hum = engine.audio().play(hum, {.loop = true});

engine.audio().set_gain(engine_hum, throttle);
engine.audio().set_pitch(engine_hum, 0.8f + throttle * 0.6f);
engine.audio().set_pan(engine_hum, pan);
bool still = engine.audio().playing(engine_hum);
engine.audio().stop(engine_hum);
```

Handles to sounds that have finished are ignored safely, so there's no need to check before calling.
`stop_all()` stops every sound.

When all 128 voices are busy, a new sound replaces the quietest one that isn't looping.

### Sounds made in code

```cpp
std::vector<f32> mono(48000 / 10);  // 0.1 s at audio::Sound::sample_rate
// ... synthesize
Asset<audio::Sound> blip = engine.assets().add("generated/blip", audio::Sound::from_mono(mono));
```

The sprites and tutorial samples synthesize their sounds this way, so they need no audio files.

## Music

Music is `audio::Music`, an Ogg Vorbis file that's decoded a little at a time while it plays, so a long track doesn't
sit decoded in memory:

```cpp
Asset<audio::Music> town = engine.assets().load<audio::Music>("music/town.ogg");

engine.audio().play_music(town);                              // loops by default, gaplessly
engine.audio().play_music(night, {.fade_in = 2.0f});          // crossfade from whatever was playing
engine.audio().stop_music(1.5f);                              // fade out over 1.5 s
bool playing = engine.audio().music_playing();
```

| `MusicParams` | Default | |
| --- | --- | --- |
| `gain` | 1 | |
| `fade_in` | 0 | Seconds; the previous track fades out over the same time |
| `loop` | `true` | Gapless: the end joins the start without a click or pause |
| `bus` | `Music` | |

Loops are gapless only if the file itself loops cleanly; export loops from your audio editor without padding.

## Buses

Every sound and track plays on a bus. The final volume is the sound's gain times its bus's gain times the master
gain.

| Bus | Default use |
| --- | --- |
| `Master` | Everything |
| `Music` | Music |
| `Effects` | Sound effects |
| `Ambience` | Background loops: wind, crowds, rain |
| `Voice` | Dialogue |
| `Ui` | Menu clicks |

```cpp
engine.audio().set_bus_gain(audio::Bus::Music, engine.settings().get("audio.music", 0.8f));
```

Buses are what volume sliders in an options menu should control. Store the values in `engine.settings()` so they're
remembered (see [saving.md](saving.md#player-settings)).

## Positional sound

There's no 3D or positional helper yet. For 2D games, panning by screen position and attenuating by distance covers
most needs:

```cpp
Vec2 rel = source - engine.camera().position;
f32 pan = clamp(rel.x / 600.0f, -1.0f, 1.0f);
f32 gain = clamp(1.0f - rel.length() / 1200.0f, 0.0f, 1.0f);
engine.audio().play(sound, {.gain = gain, .pan = pan});
```

## Without an audio device

If the machine has no audio output (a CI server, a VM without sound), initialization logs a warning and the game
runs silently; every call still works, nothing is heard. `engine.audio().active()` tells you which case you're in. `mix(out, frames)` renders audio into a
buffer directly, which the tests use.

## Hot reload

Sound and music assets reload when their files change. A voice already playing keeps the data it started with, and
the next `play` uses the new version.
