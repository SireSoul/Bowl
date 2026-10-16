# Audio

The engine mixes audio itself, on SDL's audio thread: up to 128 sounds at once, streamed music with crossfades, and
six buses with their own volume, filters and reverb, and sounds placed in the world. Every `AudioEngine` call is
thread-safe.

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
| `low_pass`, `high_pass` | 0 (off) | Filters on this sound alone, in Hz: a voice behind a door, a far-off explosion |

WAV files (anything SDL reads: PCM, float, ADPCM) and Ogg Vorbis files are supported, mono or stereo, at any sample
rate; everything is converted to the mixer's 48 kHz stereo on load. Ogg Opus files (`.opus`) work too when the engine
is built with `-DPH_OPUS=ON` (see [Opus](#opus)).

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

`set_filters(voice, low_pass, high_pass)` changes a playing sound's filters; they glide to the new values, so it
can follow gameplay every frame (the door opening, the player walking around a corner).

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

Music is `audio::Music`, an Ogg Vorbis (or Opus) file that's decoded a little at a time while it plays, so a long track doesn't
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

### Ducking

Music that dips while someone talks, and comes back after:

```cpp
engine.audio().set_ducking(audio::Bus::Music, audio::Ducking{.trigger = audio::Bus::Voice, .depth = 0.5f});
```

| `Ducking` | Default | |
| --- | --- | --- |
| `trigger` | `Voice` | The bus that makes this one dip |
| `depth` | 0.5 | How much quieter, 0 to 1 |
| `attack` | 0.08 | Seconds to dip once the trigger plays |
| `release` | 0.5 | Seconds to come back after it stops |
| `threshold` | 0.02 | How loud the trigger has to be to count |

`set_ducking(bus, std::nullopt)` turns it off; `duck_level(bus)` says how far it's dipped right now (1 is full
volume). Any bus can duck under any other: ambience under effects, effects under the UI.

### Bus effects

Each bus can filter and add reverb to everything playing through it:

```cpp
// Underwater: everything muffled.
engine.audio().set_bus_effects(audio::Bus::Master, {.low_pass = 600});
// The pause menu: music sounds like it's in the next room.
engine.audio().set_bus_effects(audio::Bus::Music, {.low_pass = 900});
// A cave: footsteps and voices ring.
engine.audio().set_bus_effects(audio::Bus::Effects, {.reverb = 0.4f, .room_size = 0.8f});
// Back to normal.
engine.audio().set_bus_effects(audio::Bus::Effects, {});
```

| `BusEffects` | Default | |
| --- | --- | --- |
| `low_pass` | 0 (off) | Hz. Cuts what's above: 400-1000 for muffled, 3000 for a wall away |
| `high_pass` | 0 (off) | Hz. Cuts what's below: 1000-2000 for a radio or a phone |
| `reverb` | 0 | 0 to 1, how much reverb is heard |
| `room_size` | 0.5 | 0 to 1, how long it rings |
| `damping` | 0.5 | 0 to 1, how fast its high frequencies die away |

Changes glide over about 50 ms, so setting effects every frame from gameplay (how deep underwater the player is) is
fine and doesn't click. Effects apply before the bus's volume; the master bus's effects apply to everything. The
reverb is a Freeverb: eight combs and four allpasses per side, cheap enough for every bus at once.

## Positional sound

Sounds can be placed in the world. Their volume falls off with distance from the listener, and they pan to the side
they're on:

```cpp
// every frame: hear from the camera (or the player)
engine.audio().set_listener(engine.camera().position);

// a one-off at a place
engine.audio().play_at(splash, rock_position);

// a loop that moves
audio::Voice hum = engine.audio().play_at(hum_sound, cart.position, {.loop = true}, {.near = 32, .far = 600});
engine.audio().set_position(hum, cart.position);   // as it moves
```

| `Attenuation` | Default | |
| --- | --- | --- |
| `near` | 64 | World units: full volume within this distance |
| `far` | 1024 | Silent beyond this |
| `pan_distance` | 512 | Fully to one side once this far left or right |

Between `near` and `far` the volume falls off quickly at first and then gently. Gain and pan are worked out as the
audio plays, so moving the listener or the sound needs nothing more. `audio::spatialize(source, listener,
attenuation)` returns the same gain and pan, for sounds you start with `play`.

## Opus

Opus sounds better than Vorbis at the same size, especially for voices and at low bitrates. Configure with
`-DPH_OPUS=ON` to load `.opus` files as sounds and music; it adds libogg, libopus and opusfile to the build.
`audio::opus_supported()` says whether it's in. Without it, loading an Opus file logs an error that says how to
turn it on.

## Without an audio device

If the machine has no audio output (a CI server, a VM without sound), initialization logs a warning and the game
runs silently; every call still works, nothing is heard. `engine.audio().active()` tells you which case you're in. `mix(out, frames)` renders audio into a
buffer directly, which the tests use.

## Hot reload

Sound and music assets reload when their files change. A voice already playing keeps the data it started with, and
the next `play` uses the new version.
