# Sprite sheets and animation

Animated characters and effects come from sprite sheets: one texture holding many frames, plus named animations
over ranges of those frames. The engine reads sheets exported from [Aseprite](https://www.aseprite.org), and can also
cut a uniform grid.

## Exporting from Aseprite

1. Tag each animation in the timeline (`idle`, `walk`, `attack`, ...). The tag's name, frame range, direction
   (forward, reverse, ping-pong, ping-pong reverse) and repeat count are all used.
2. File > Export Sprite Sheet. On the **Output** tab, enable **JSON Data** and pick either **Hash** or **Array**.
   Under meta, enable **Tags**.
3. Trimming (**Trim Cels**, **Trim Sprite**) is supported and saves texture space; the engine puts trimmed frames
   back where they belong.
4. Save the PNG and JSON next to each other in your assets folder.

Frame durations come from Aseprite's per-frame durations.

## Loading and drawing

```cpp
#include "ph/gfx/sprite_sheet.hpp"

Asset<gfx::SpriteSheet> hero = engine.assets().load<gfx::SpriteSheet>("characters/hero.json");
```

The PNG named in the JSON loads with it. Re-exporting from Aseprite updates the running game: the sheet reloads
when either file changes.

Draw a frame with `draw_frame`, which handles trimming and the pivot:

```cpp
gfx::draw_frame(batch, *hero, frame_index, position);                     // pivot at position
gfx::draw_frame(batch, *hero, frame_index, position, {-1, 1});            // mirrored horizontally
gfx::draw_frame(batch, *hero, frame_index, position, {2, 2}, Color::white(), position.y, rotation);
```

`sheet.pivot` is where the frame's anchor is, as a fraction of the full (untrimmed) frame. The default is the center;
`{0.5f, 1.0f}` puts the bottom center (the feet) on the position, which is what top-down and isometric characters
want:

```cpp
hero->pivot = {0.5f, 1.0f};
```

## Playing animations

`gfx::SpriteAnimator` is a plain struct that tracks which animation is playing and which frame it's on. Make it a
component and step it in a system:

```cpp
gfx::SpriteAnimator anim;
anim.sheet = hero;
anim.play("walk");
world.create(Position{...}, anim);

// in init:
engine.frame_schedule().par_each<gfx::SpriteAnimator>("animate",
    [](ecs::SystemContext& ctx, ecs::Entity, gfx::SpriteAnimator& a) { a.update(ctx.dt); });

// in render:
world.each<const Position, const gfx::SpriteAnimator>([&](const Position& p, const gfx::SpriteAnimator& a) {
    gfx::draw_frame(batch, *a.sheet, a.frame, p.value, {1, 1}, Color::white(), p.value.y);
});
```

| Member | |
| --- | --- |
| `play(name, restart = false)` | Start an animation by name. Without `restart`, calling it every frame with the same name keeps it running. Returns `false` if the sheet has no such animation |
| `update(dt)` | Advance time and frames |
| `frame` | Index into `sheet->frames`, to pass to `draw_frame` |
| `speed` | Playback speed multiplier |
| `playing` | Set to `false` to pause |
| `finished` | A non-looping animation (tag repeat count above 0) reached its end; it holds the last frame |
| `frame_changed` | `true` for the update that moved to a new frame: the moment to play a footstep or apply a hit |
| `loops_done` | Completed loops |

Animation is visual, so it usually belongs in the frame schedule. If an animation drives gameplay (a hit lands on
frame 3), step it in the fixed schedule instead, so it happens at the same tick on every machine.

## Sheets without Aseprite

`SpriteSheet::grid` cuts a texture into equal cells, left to right and top to bottom, as one animation named
`"default"`:

```cpp
auto sheet = std::make_unique<gfx::SpriteSheet>(
    gfx::SpriteSheet::grid(engine.assets().load<gfx::Texture>("fx/explosion.png"), 64, 64, 12, 0.05f));
sheet->animations.push_back({"loop", 4, 11, gfx::SpriteAnimation::Direction::PingPong});
Asset<gfx::SpriteSheet> explosion = engine.assets().add("fx/explosion.sheet", std::move(sheet));
```

A sheet loaded from Aseprite JSON without tags also gets a single `"default"` animation over all frames.

## What's not there yet

Aseprite slices (for hitboxes and nine-slice panels), animation events by name, and a texture atlas packer are on
the [roadmap](../roadmap.md).
