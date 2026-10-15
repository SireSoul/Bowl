# Bowl Editor

Bowl Editor lays out levels: it places art and markers in a `.world` file that the game loads. It draws with the
game's own renderer, so what you see is what the game shows.

<p align="center"><img src="../images/editor.png" width="90%" alt="Bowl Editor with a level open and a pillar selected"></p>

## Starting it

The editor is built with the engine in dev builds (`build/dev/editor/bowl-editor`).

```bash
./build/dev/editor/bowl-editor assets/levels/town.world --game build/dev/my_game/libmy_game_code.so
```

| Argument | |
| --- | --- |
| `level.world` | The level to open. Without one, it starts with an empty, untitled level |
| `--assets <folder>` | Where the art is. By default, the `assets` folder the level is in, or `./assets` |
| `--game <library>` | The game's hot-reload code library, so the game's own components can be added and edited (macOS and Linux) |
| `--select <name>` | Select the thing with this name at startup |
| `--resave` | Open the level, save it in the current format and quit |

The first time it opens, a short welcome explains the basics. Rest the mouse on any button for what it does and its
key; **F1** lists every key.

## The screen

- **Top bar:** new, open and save; the tools (Select, Place, Move the view); undo and redo; the grid, snapping and
  the grid size; help.
- **Left: Art and Outline.** Art shows every picture in the assets folder as a thumbnail, grouped by folder, with a
  search box. Outline lists everything in the level, with what's inside what.
- **Right: the inspector.** What's selected: its name, where it is, its art, and any other components.
- **Bottom: the status line.** What a click would do right now, short messages ("Saved town.world"), the zoom and
  the mouse's position in the world.

## Placing things

Pick a picture on the Art tab, then click the level: every click places another. A see-through preview follows the
mouse. **Esc** goes back to selecting. You can also drag a picture from the shelf straight into the level.

- Things snap to the grid. Hold **Alt** to place one exactly where the mouse is.
- Pictures stand on their **feet**: the middle of their bottom edge. Change that in the Art card ("Stands on").
- **+ Empty thing** places a marker with nothing drawn: spawn points, triggers, the anchor of a group.

## Selecting and arranging

| | |
| --- | --- |
| Click | Select |
| Shift-click | Add to or take from the selection |
| Drag on empty ground | Select everything in the box |
| Drag a thing | Move it and everything selected, snapped |
| Alt-drag | Move copies, leaving the originals in place |
| Arrows (Shift) | Nudge one unit (a grid step) |
| Q / E (Shift) | Turn 15 degrees (1 degree) |
| Ctrl+D, Ctrl+C, Ctrl+X, Ctrl+V | Duplicate, copy, cut, paste at the mouse |
| Ctrl+G, Ctrl+Shift+G | Group, ungroup |
| Delete | Remove, with anything inside |

With several things selected, the inspector offers **Group**, **Duplicate**, **Delete** and lining them up by their
left, center, right, top, middle or bottom edges.

**Groups.** A group is a thing with others inside it; moving, turning or scaling it carries them along. In the
Outline, drag one thing onto another to put it inside, or onto the empty space below to take it out. Either way it
stays where it is in the world.

## Looking around

The right or middle mouse button drags the view, and so does the left button with the Move tool (H) or while holding
Space. The wheel zooms toward the mouse. **F** shows the selection, **Home** the whole level. Double-clicking a row in
the Outline shows that thing.

## The inspector

- **Name**, at the top: shown in the Outline, and a way for the game to find things.
- **Place:** position, turn (in degrees) and scale, with Mirror and Reset.
- **Art:** the picture (Change..., or drop one from the shelf onto the preview), a tint color, shown or hidden, natural
  size or a size of its own, where it stands, whether it sorts by height (lower on screen draws in front), and its
  draw order.
- **Other components** each get a card with their fields and a remove button. **+ Add** gives the thing any component
  the editor knows: the engine's (Art, Particles) and, with `--game`, the game's own.

### The game's own components

A level often carries components only the game knows: a spawn point's kind, a door's destination. Start the editor
with `--game` pointing at the game's hot-reload library (`HOT_RELOAD` in `ph_add_game`) and those components get
proper fields, and can be added from **+ Add**. The editor never runs the game's code; loading the library only
registers its `PH_COMPONENT` types.

Without `--game`, those components are kept as JSON: shown as text you can edit, and written back exactly when
saving. Opening and saving a level never loses anything.

On Windows, `--game` isn't supported yet; game components stay as JSON there.

## Saving and safety

- **Ctrl+S** saves, **Ctrl+Shift+S** saves under a new name. A running game picks the level up by itself (it's an
  ordinary hot-reloading asset).
- **Undo and redo** (Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y) cover everything, keeping what was selected.
- New and Open ask before throwing away unsaved changes.
- When the editor closes with unsaved changes, they're kept in `<level>.world.autosave` and offered back the next time
  the level is opened.
- Preferences (grid, snapping, grid size) are remembered in the editor's own settings file in your user folder.

## Loading levels in the game

A `.world` file is an asset:

```cpp
Asset<ecs::WorldFile> town = engine.assets().load<ecs::WorldFile>("levels/town.world");
serial::Context ctx{&engine.assets()};
ecs::load_entities(engine.world(), town->document, ctx);
```

Draw what the level placed with `gfx::draw_sprites(world, batch)` after the transform systems have run
(`ecs::add_transform_systems(engine.frame_schedule())`), or draw entities your own way. See
[saving.md](saving.md#world-files) for the file format.

## The engine overlay

The engine's own overlay (profiler, systems, log) is on **F12** in the editor, since F1 is the editor's help.

## Coming next

A **Data** workspace for editing the game's JSON files in place, keeping their comments, and a **Paint** studio for
pixel art drawn right where it stands in the level. See the [roadmap](../roadmap.md).
