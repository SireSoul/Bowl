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

`position` is the top of the text block: its top-left corner for left-aligned text, its top center for centered text,
its top-right for right-aligned.

## Sharp text: raster_scale

Fonts are rasterized at `size * raster_scale` pixels. Set `raster_scale` to how many real screen pixels one world
unit covers:

- **UI** with a screen camera: `engine.window().pixel_density()` (2 on most high-DPI laptop screens).
- **Text in the world**: also multiply by the camera's zoom: `pixel_density() * camera.zoom`.

A `raster_scale` too low looks blurry; too high wastes atlas space. Sizes are rounded to whole pixels, so text that
smoothly changes size (a zoom animation) rasterizes a new size every pixel step. That's fine for a moment, but avoid
animating text size continuously for long periods.

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

## Fonts in more detail

```cpp
gfx::Font& f = *font;
gfx::Font::Metrics m = f.metrics(24);    // ascent, descent (negative), line_gap, line_height()
f32 k = f.kerning('A', 'V', 24);
const gfx::Font::Glyph& g = f.glyph('A', 24);
```

Glyphs are packed into 1024x1024 atlas pages, added as needed. When new glyphs appear, only the rectangle they
occupy is uploaded to the GPU. A missing glyph is drawn as `?`.

## Localized text

Get strings from the localization tables rather than hard-coding them: `engine.i18n().tr("menu.start")`. See
[localization.md](localization.md).

## Limitations

No outlines, shadows or signed-distance-field rendering yet, and no fallback fonts for characters the font lacks.
Both are on the [roadmap](../roadmap.md). For a shadow today, draw the text twice, offset and darker first.
