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
#include "bowl/gfx/sprite_sheet.hpp"

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
| `frame_changed` | `true` for the update that moved to a new frame |
| `fired(event)` | Whether the last update (or `play`) reached a frame with this event; see below |
| `loops_done` | Completed loops |

Animation is visual, so it usually belongs in the frame schedule. If an animation drives gameplay (a hit lands on
frame 3), step it in the fixed schedule instead, so it happens at the same tick on every machine.

## Events

Name the moments that matter in Aseprite instead of hard-coding frame numbers: in the tag's properties, put the events
in its **User Data** text, as `<frame>:<name>` with the frame counted from the tag's first frame:

```
2:step 6:step 4:dust
```

Then ask the animator:

```cpp
if (anim.fired("step")) engine.audio().play(footstep);
if (anim.fired("hit")) deal_damage(target);
```

`fired` covers every frame the update passed through, so nothing is missed at low frame rates or high speeds. Events
are part of the sheet, so re-exporting with moved events changes the game without touching code.

## Slices: hitboxes, panels, attachment points

Aseprite's **slices** are named rectangles on the sprite, and they can change from frame to frame. The engine loads
them all (enable **Slices** in the export's meta options):

```cpp
// A hitbox that follows the animation, in world coordinates, for the frame drawn at `position`:
if (auto box = gfx::slice_rect(*sheet, "hitbox", anim.frame, position, scale)) check_hits(*box);

// A nine-slice panel: give the slice a 9-slices center in Aseprite, then draw it at any size.
gfx::draw_nine_slice(ui_batch, *panel_sheet, "frame", 0, {40, 40, 300, 120});

const gfx::SpriteSlice* s = sheet->slice("hand");      // with keys per frame, a pivot and user data
```

`slice_rect` accounts for the sheet's pivot and for mirroring (negative scale), so a hitbox drawn for a character
facing left is where the character's sword is.

## Frames by name

`sheet.frame_index("door_open")` finds a frame by its exported name (with or without the file extension), which is
the natural way to use an atlas of separate pictures.

## Packing pictures into an atlas: bowl-atlas

Many small pictures drawn from one texture cost one draw call instead of many. `bowl-atlas` packs a folder of PNGs into
one atlas image and writes JSON next to it that `SpriteSheet` loads like an Aseprite export:

```bash
bowl-atlas art/props -o assets/props.png
```

- Each picture becomes a frame named after its file (with its subfolder: `"trees/oak"`).
- Transparent edges are trimmed and remembered, so pictures still line up; `--no-trim` keeps them.
- Files named `walk_0.png`, `walk_1.png`, ... (or `walk 0.png`) also become an animation called `walk`; `--fps` sets
  its speed (10 by default).
- `--padding` (1) keeps pictures apart so filtering doesn't bleed; `--max` (4096) caps the atlas size.

```cpp
Asset<gfx::SpriteSheet> props = engine.assets().load<gfx::SpriteSheet>("props.json");
gfx::draw_frame(batch, *props, props->frame_index("trees/oak"), position);
```

The tool is in `build/<preset>/tools/atlas/`. Its packing code is the `bowl_atlas` library, for tools of your own.

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
