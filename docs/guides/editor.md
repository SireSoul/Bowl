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
| `--game <library>` | The game's hot-reload code library, so the game's own components can be added and edited. On Windows, `<game>_editor_code.dll` |
| `--data`, `--paint` | Start in the Data or Paint workspace with its first file open |
| `--select <name>` | Select the thing with this name at startup |
| `--resave` | Open the level, save it in the current format and quit |
| `--attach [profiler\|systems\|entities]` | Start in the Game workspace, attached to the first game running on this computer, on that pane |

The first time it opens, a short welcome explains the basics. Rest the mouse on any button for what it does and its
key; **F1** lists every key.

## The screen

The editor has three workspaces, switched at the top left: **Level** (Ctrl+1) lays out levels, **Data** (Ctrl+2) edits
the game's data files, **Paint** (Ctrl+3) paints its art. The rest of this section is the Level workspace.

- **Top bar:** new, open and save; the tools (Select, Place, Move the view, Brush, Region); undo and redo; the grid,
  snapping and the grid size; showing collision shapes; help.
- **Left: Art and Outline.** Art shows every picture, Tiled map (`.tmj`, `.tmx`) and prefab (`.prefab`) in the assets
  folder, grouped by folder, with a search box. Outline lists everything in the level, with what's inside what, and
  an eye and a padlock on each row (see [Hiding and locking](#hiding-and-locking)).
- **Right: the inspector.** What's selected: its name, where it is, its art, and any other components.
- **Bottom: the status line.** What a click would do right now, short messages ("Saved town.world"), the zoom and
  the mouse's position in the world.

## Placing things

Pick a picture on the Art tab, then click the level: every click places another. A see-through preview follows the
mouse. **Esc** goes back to selecting. You can also drag a picture from the shelf straight into the level.

- Things snap to the grid. Hold **Alt** to place one exactly where the mouse is.
- Pictures stand on their **feet**: the middle of their bottom edge. Change that in the Art card ("Stands on").
- **+ Empty thing** places a marker with nothing drawn: spawn points, triggers, the anchor of a group.
- A **prefab** places a copy of its entities, its first one where you click. The copy is the level's own from then on:
  changing the prefab file later doesn't change copies already placed.

### Maps

A Tiled map from the shelf goes in the level as a thing with a `gfx::Map` component, its corner where you click (the
top-left of its bounding box, as in Tiled). It starts **locked**, so clicks go to what you put on it rather than
selecting the map; unlock it in the Outline to move it.

With a map in the level, snapping follows the map's cells instead of the square grid: things land in the middle of a
cell (a diamond on isometric maps), and the cell under the mouse is outlined. Tall layers sort with the art you place
by their bottom edge, so a character in front of a pillar draws in front of it, the same as in the game.

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

## Hiding and locking

Each row in the Outline has an eye and a padlock, and the right-click menu has the same:

- **Hide** takes a thing out of the editor's view, to work on what's behind it. Only the editor hides it: the game
  still shows it. (Whether the *game* shows a picture is the Art card's **Shown**.)
- **Lock** makes clicks in the scene go past a thing (and box selections leave it out), so a backdrop doesn't get
  picked or dragged by accident. Select it in the Outline to change it.

Both apply to everything inside a group, are saved with the level under `"editor"` (games ignore it), and can be
undone.

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
with `--game` pointing at the game's hot-reload library (`HOT_RELOAD` in `bowl_add_game`) and those components get
proper fields, and can be added from **+ Add**. The editor never runs the game's code; loading the library only
registers its `BOWL_COMPONENT` types.

Without `--game`, those components are kept as JSON: shown as text you can edit, and written back exactly when
saving. Opening and saving a level never loses anything.

On Windows a game's code library is tied to the game's own executable (it imports the engine from it), so the build
makes a second copy for the editor: `<game>_editor_code.dll`, next to the editor. Pass that to `--game`.

## Regions and collision shapes

<p align="center"><img src="../images/editor-regions.png" width="80%" alt="A region and a round collider on the level, with the region's handles"></p>

Every `physics::Collider` in the level is drawn on it: triggers (regions) tinted teal, solid shapes outlined in orange.
**C** or the eye button hides them.

- **The Region tool (R):** drag out an area, or click for a square one. It becomes a thing named "Region 1" (and so on)
  with a trigger collider: rename it, and the game finds it by name, or add the game's own components to it (a door's
  destination, a spawn area's kind).
- **Resizing:** select a box or circle collider and drag its handles; the opposite corner stays put, and the grid
  applies.
- Everything else about a collider (its kind, a polygon's points, triggers, layers) is in its card on the right.

## Painting where it stands

The **Brush tool (B)** paints on a picture right in the level: click and drag on placed art, and the pixel under the
mouse changes, through the thing's position, rotation, scale and art region. Every placed copy of that picture
changes with it, and a running game sees it too. The colors and tools are the Paint workspace's.

## The Data workspace

<p align="center"><img src="../images/editor-data.png" width="80%" alt="The Data workspace: a JSON file as a tree and as text"></p>

Every `.json` and `.prefab` file in the assets folder, on the left. The open file shows twice: as a tree of values
you can change, and as its text.

- Numbers drag or take typing, text is a text field, on/off is a checkbox. Lists have **+** to add an item (a copy of
  the last) and every value has **x** to remove it; objects have a **+ new key** box. An empty value offers a type.
- **Changes keep the file as it was written:** comments, spacing, key order and trailing commas stay, because only
  the value that changed is rewritten (`json::TextDocument` does this, and is there for your own tools too).
- The text can be edited directly as well. While it doesn't parse, the tree shows where the mistake is.
- **Ctrl+S** saves, **Ctrl+Z** and **Ctrl+Shift+Z** undo and redo, **Revert** goes back to the file on disk. A
  running game reloads the file when it's saved.

## The Game workspace

The **Game** tab (**Ctrl+4**) attaches the editor to a game running on this computer, from a dev build: the game's
own window keeps playing, and this one looks inside it. Running games are listed on the left; click one to attach,
click it again to let go. Nothing on a network can attach: the game listens on this computer only, with a key in a file
only you can read.

- **Profiler:** frame times as a graph, and the profiler's zones averaged over the last second, slowest first. Pause,
  step one fixed tick, or slow the game down or speed it up.
- **Systems:** both schedules, each system with its batch, its time and a switch to turn it off in the game. A system
  in the accent color starts a new batch: rest the mouse on it for why. **Access by component** is the same as a table.
  The editor keeps the last schedules a game sent, so this pane, and an **In the game** note under a thing's components
  in the Level tab ("Transform: written by integrate; read by draw"), still say what the game does when it isn't
  running.
- **Entities:** every entity, by name or what it's made of, a page at a time, with a filter that matches names and
  component names. Pick one and its components show as values, changing as the game runs; change one and the game's
  changes with it. Components that point at other entities are shown but not edited, since their JSON holds those
  references as nothing.

## The Paint workspace

<p align="center"><img src="../images/editor-paint.png" width="80%" alt="The Paint workspace with a picture, tools and its palette"></p>

Pick a picture on the left to paint it on a big canvas with a pixel grid.

| Tool | Key | |
| --- | --- | --- |
| Pencil | B | Paints pixels; fast strokes have no gaps |
| Eraser | E | Makes pixels transparent |
| Fill | F | Fills the area of one color |
| Pick | I, or hold Alt | Takes a color from the picture |

The palette starts with the colors already in the picture, most used first. The wheel zooms toward the mouse,
right-drag moves. **Ctrl+Z** / **Ctrl+Shift+Z** undo and redo strokes, **Ctrl+S** writes the PNG. Every stroke shows in
the level (and a running game) at once; switching pictures saves the one you were painting.
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

Draw what the level placed with `gfx::draw_maps(world, batch, time)` and `gfx::draw_sprites(world, batch)`, into
the same batch so they sort together, after the transform systems have run
(`ecs::add_transform_systems(engine.frame_schedule())`), or draw entities your own way. See
[saving.md](saving.md#world-files) for the file format.

## The engine overlay

The engine's own overlay (profiler, systems, log) is on **F12** in the editor, since F1 is the editor's help.
