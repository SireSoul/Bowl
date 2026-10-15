# Text

Text is drawn from TrueType and OpenType fonts into ordinary sprite batches. Glyphs are rasterized the first time
they're used, at the exact pixel size they're drawn, so text is sharp at any size and on any screen without a
preprocessing step.

## Drawing text

```cpp
#include "ph/gfx/text.hpp"

Asset<gfx::Font> font = engine.assets().load<gfx::Font>("fonts/Alegreya-Regular.ttf");

hud.begin(gfx::Camera2D::screen(engine.window().size()), gfx::SpriteSort::None);
gfx::TextStyle style{.size = 24, .color = Color::hex(0xf2f0ea), .raster_scale = engine.window().pixel_density()};
gfx::draw_text(hud, *font, "Hello, world", {24, 20}, style);
engine.renderer().submit_ui(hud);
```

`draw_text` takes UTF-8, so any script the font covers works. `\n` starts a new line. It returns the size of the
block it drew; `gfx::measure_text(font, text, style)` returns the same size without drawing, for layout.

The engine ships Roboto Medium as `engine/fonts/Roboto-Medium.ttf` (Apache 2.0). Your own fonts go in your assets
folder; check that their license allows embedding in a game.

## TextStyle

| Field | Default | |
| --- | --- | --- |
| `size` | 16 | Font size (ascent to descent) in world units; for a screen camera, logical pixels |
| `color` | white | |
| `align` | `Left` | `Left`, `Center` or `Right`: which point of each line `position.x` refers to |
| `max_width` | 0 | When above 0, wrap words to this width |
| `line_spacing` | 1 | Multiplier on the font's line height |
| `raster_scale` | 1 | Screen pixels per world unit, so glyphs are rasterized at screen resolution |
| `max_glyphs` | -1 | Draw only the first N visible characters (with the full text's layout); -1 for all |
| `outline`, `outline_color` | 0, black | An outline this many world units thick around each letter |
| `shadow`, `shadow_color` | (0, 0), black at 60% | A copy of the text moved by this offset, drawn underneath |
| `markup`, `time` | false, 0 | Read color and effect tags in the text; see below |
| `sdf` | false | Draw from distance fields; see below |

`position` is the top of the text block: its top-left corner for left-aligned text, its top center for centered text,
its top-right for right-aligned.

## Sharp text: raster_scale

Fonts are rasterized at `size * raster_scale` pixels. Set `raster_scale` to how many real screen pixels one world
unit covers:

- **UI** with a screen camera: `engine.window().pixel_density()` (2 on most high-DPI laptop screens).
- **Text in the world**: also multiply by the camera's zoom: `pixel_density() * camera.zoom`.

A `raster_scale` too low looks blurry; too high wastes atlas space. Sizes are rounded to whole pixels, so text that
smoothly changes size (a zoom animation) rasterizes a new size every pixel step. For text whose size keeps changing,
use [distance-field text](#distance-field-text) instead.

## Wrapping and alignment

```cpp
gfx::TextStyle body{.size = 16, .align = gfx::TextAlign::Center, .max_width = 520,
                    .raster_scale = engine.window().pixel_density()};
gfx::draw_text(ui, *font, long_help_text, {screen.x * 0.5f, screen.y - 80}, body);
```

Lines break at the last space that fits. A single word wider than `max_width` is broken wherever it runs out of
room.

## Typewriter reveals

`max_glyphs` draws only the first N visible characters but keeps the layout of the whole text, so words don't jump
from line to line as they appear. `ui::Typewriter` drives it over time and is what the dialogue box uses:

```cpp
ui::Typewriter line;
line.start("The lighthouse keeper hasn't been seen in days.");
line.update(dt);                                   // every frame
style.max_glyphs = line.visible_glyphs();
if (line.done()) { /* show a "continue" marker */ }
```

See [ui.md](ui.md#dialogue).

## Outlines and shadows

```cpp
gfx::TextStyle title{.size = 32, .raster_scale = engine.window().pixel_density()};
title.outline = 1.5f;              // world units
title.outline_color = Color::hex(0x1a1f3d);
title.shadow = {2, 3};
gfx::draw_text(hud, *font, "Day 1", {24, 20}, title);
```

The shadow is drawn first, then the outline, then the letters, all in the same batch. Outlines are copies of the
letters drawn around them (8 copies, or 16 for outlines wider than a pixel and a half), which looks right up to a few
pixels. For thick outlines use distance-field text. Neither changes the layout or the size `draw_text` returns.

## Rich text markup

With `markup = true`, tags in the text change how parts of it look:

```cpp
style.markup = true;
style.time = static_cast<f32>(engine.time());   // drives the effects
gfx::draw_text(ui, *font, "You found [color=#ffcc00]a gold key[/color]! [wave]Wonderful.[/wave]", at, style);
```

| Tag | |
| --- | --- |
| `[color=#rgb]`, `[color=#rrggbb]`, `[color=#rrggbbaa]` ... `[/color]` | Text color (multiplied by `style.color`'s alpha) |
| `[wave]` ... `[/wave]` | Letters bob up and down |
| `[shake]` ... `[/shake]` | Letters jitter in place |
| `[[` | A literal `[` |

Tags nest. Anything in brackets that isn't a known tag is drawn as text. Tags take no space, and `max_glyphs` counts
only the characters that are drawn, so typewriter reveals work with markup. `gfx::strip_markup(text)` returns the text
without tags.

## Distance-field text

With `sdf = true`, glyphs come from signed distance fields made once at 48 pixels. They stay sharp at any size,
scale and rotation without rasterizing again, which suits titles that pulse, text in a zooming world, and thick
outlines. Draw them in a batch begun with `gfx::sdf_text_material`:

```cpp
// once
gfx::Material sdf_look = gfx::sdf_text_material(engine.assets(), {.outline_color = Color::black(), .outline = 0.25f});

// every frame
titles.begin(gfx::Camera2D::screen(screen), gfx::SpriteSort::None, gfx::BlendMode::Alpha, &sdf_look);
gfx::TextStyle big{.size = 40 + 8 * std::sin(t * 2), .color = Color::hex(0xffe066)};
big.sdf = true;
gfx::draw_text(titles, *font, "Bowl", {24, 92}, big);
engine.renderer().submit_ui(titles);
```

The material's `outline` is a fraction of the glyph's falloff (0 to about 0.4) and `softness` blurs the edge for
glows. `raster_scale` isn't needed. Below about 12 pixels, bitmap text is crisper; use SDF for large and moving text.

## Fallback fonts

A font only has the characters its designer drew. For Japanese, Chinese, Korean, emoji or symbols, give it fonts to
fall back on:

```cpp
font->set_fallbacks({engine.assets().load<gfx::Font>("fonts/NotoSansJP-Regular.otf"),
                     engine.assets().load<gfx::Font>("fonts/NotoEmoji-Regular.ttf")});
```

Each character comes from the first font that has it, scaled to match. Characters none of them have are drawn as `?`.
`font->has_glyph(cp)` tells you whether a character will draw. Color emoji fonts (bitmap or COLR glyphs) aren't
supported; use a monochrome emoji font.

## Fonts in more detail

```cpp
gfx::Font& f = *font;
gfx::Font::Metrics m = f.metrics(24);    // ascent, descent (negative), line_gap, line_height()
f32 k = f.kerning('A', 'V', 24);
f32 w = f.advance('A', 24);              // without rasterizing
const gfx::Font::Glyph& g = f.glyph('A', 24);
```

Glyphs are packed into 1024x1024 atlas pages, added as needed. When new glyphs appear, only the rectangle they
occupy is uploaded to the GPU. Distance-field glyphs have their own pages.

`gfx::layout_text` returns the characters `draw_text` would draw (page, source rectangle, position, size and color for
each), for drawing text your own way: per-letter animation, text on a curve, a custom shader.

## Localized text

Get strings from the localization tables rather than hard-coding them: `engine.i18n().tr("menu.start")`. See
[localization.md](localization.md).
