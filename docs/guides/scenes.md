# Scenes

A scene is one screen of a game: the title, the game itself, an inventory, a pause menu. The engine keeps a stack of
them in `engine.scenes()`. The top scene is the active one; scenes below can keep drawing (a menu over the paused
game) or even keep running (a HUD over live gameplay).

Using scenes is optional. A game can do everything in its `Game` class; the isometric sample does. Scenes help once a
game has several screens with their own setup and teardown.

## A scene

```cpp
class PauseMenu final : public bowl::Scene {
public:
    void enter(Engine& engine) override { engine.set_paused(true); }
    void exit(Engine& engine) override { engine.set_paused(false); }
    void update(Engine& engine, f32 dt) override {
        if (engine.input().pressed("back")) engine.scenes().pop();
    }
    void render(Engine& engine) override { /* draw the menu with the UI */ }
    bool transparent() const override { return true; } // the game shows underneath
};
```

| Callback | When |
| --- | --- |
| `enter` | Pushed, or became the replacement |
| `exit` | Popped or replaced |
| `pause` | Another scene was pushed on top of this one |
| `resume` | The scene on top was popped |
| `fixed_update`, `update`, `render`, `dev_ui` | Like the `Game` callbacks; scenes run after the game's own |
| `transparent()` | Return `true` to have the scene below drawn too |
| `passes_updates()` | Return `true` to have the scene below keep updating too |

`dev_ui` is called for the top scene only.

## The stack

```cpp
engine.scenes().push(std::make_unique<PauseMenu>());           // on top
engine.scenes().pop();                                         // remove the top
engine.scenes().replace(std::make_unique<GameOver>());         // swap the top
engine.scenes().clear();                                       // remove all
Scene* top = engine.scenes().top();
```

Changes requested during a frame happen at the end of that frame, so a scene can safely replace or pop itself from
inside its own `update`.

### What runs

- **Updates** go to the top scene, plus each scene below it for as long as the scenes above return `true` from
  `passes_updates()`.
- **Drawing** starts at the highest scene that isn't transparent and goes up, so a transparent menu draws after (on
  top of) the scene below it.

## Fades

`push`, `pop` and `replace` take an optional fade duration in seconds:

```cpp
engine.scenes().replace(std::make_unique<GameplayScene>(), 0.4f);
```

The screen fades to `scenes().fade_color` (black by default) over that time, the change happens while it's fully
covered, then it fades back in over the same time. While fading out, fixed updates are held so the world stays still;
frame updates and drawing continue.

`scenes().transitioning()` is `true` during a fade. Check it before requesting another change from a button or key
press, because the scene being left is still drawn and still takes input until the fade covers it:

```cpp
if (ui.button("Play") && !engine.scenes().transitioning())
    engine.scenes().replace(std::make_unique<PlayScene>(), 0.3f);
```

## Sharing things between scenes

The world, the schedules, input bindings, assets and settings all belong to the engine, not to a scene. A common
split:

- `Game::init`: input bindings, systems, resources and assets every scene uses.
- `Scene::enter`: create the entities this screen needs, load its assets, set post-processing.
- `Scene::exit`: destroy those entities, restore settings it changed.

Systems run whatever scene is on top. If a system should only run during gameplay, give gameplay entities a
component the system requires, or check a resource that says which mode the game is in.

The [tutorial](../tutorial.md#11-a-title-screen) builds a two-scene game this way.
