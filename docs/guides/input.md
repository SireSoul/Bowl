# Input

`engine.input()` reads the keyboard, the mouse and up to four gamepads. Games normally don't ask about keys directly:
they bind named *actions* to keys and buttons, and ask about the actions. That way rebinding and gamepads work without
extra code, and an action can have several bindings.

## Actions

Bind actions in `Game::init`:

```cpp
Input& input = engine.input();
input.bind("jump", {InputBinding::key(Key::Space), InputBinding::button(GamepadButton::South)});
input.bind("left", {InputBinding::key(Key::A), InputBinding::key(Key::Left), InputBinding::axis(GamepadAxis::LeftX, -1)});
input.bind("right", {InputBinding::key(Key::D), InputBinding::key(Key::Right), InputBinding::axis(GamepadAxis::LeftX, +1)});
input.bind("up", {InputBinding::key(Key::W), InputBinding::key(Key::Up), InputBinding::axis(GamepadAxis::LeftY, -1)});
input.bind("down", {InputBinding::key(Key::S), InputBinding::key(Key::Down), InputBinding::axis(GamepadAxis::LeftY, +1)});
input.bind("shoot", {InputBinding::mouse(MouseButton::Left), InputBinding::axis(GamepadAxis::RightTrigger, +1)});
```

Then read them anywhere:

```cpp
if (input.pressed("jump")) { /* this tick / frame only */ }
if (input.down("shoot")) { /* held */ }
if (input.released("jump")) { /* let go */ }
f32 throttle = input.value("shoot");                        // 0..1, analog for sticks and triggers
Vec2 move = input.vector("left", "right", "up", "down");    // length at most 1, with a dead zone
```

`vector` combines four actions into a direction. Diagonals are clamped to length 1, so moving diagonally isn't
faster, and a radial dead zone (0.15 by default, the last argument) ignores stick drift.

### Kinds of bindings

| Binding | |
| --- | --- |
| `InputBinding::key(Key::Space)` | A key. Keys are *physical positions* (SDL scancodes): WASD stays where WASD is on AZERTY keyboards |
| `InputBinding::mouse(MouseButton::Right)` | A mouse button |
| `InputBinding::button(GamepadButton::South, pad)` | A gamepad button; `pad` is -1 (any gamepad, the default) or 0..3 |
| `InputBinding::axis(GamepadAxis::LeftX, -1, pad, threshold)` | One half of an axis: `-1` or `+1`. Counts as "down" past `threshold` (0.5) |

Gamepad buttons have positional names: `South` is A on Xbox, Cross on PlayStation, B on Nintendo. `East`, `West` and
`North` follow from there.

`bind` with an action that exists *adds* bindings; `unbind(action)` removes them all. `has_action`, and
`bindings(action)`, inspect what's there.

## Edges and fixed ticks

`pressed` and `released` are *edges*: true for one update. Which update depends on where you ask:

- inside `fixed_update` and fixed systems, they mean "since the previous fixed tick";
- in `update`, `render` and frame systems, "since the previous frame".

A frame can run zero, one or several fixed ticks. Without this, a tap made during a frame that runs no tick would be
lost, and one made during a frame that runs two ticks would count twice. Here it's seen exactly once in each context.

## Raw devices

When you do need specific devices:

```cpp
input.down(Key::LeftShift);
input.pressed(Key::F5);
input.pressed(MouseButton::Left);
Vec2 cursor = input.mouse_position();       // window coordinates in logical points
Vec2 moved = input.mouse_delta();           // this frame
f32 scroll = input.wheel();                 // this frame
bool pad = input.gamepad_connected(0);
f32 lx = input.axis(GamepadAxis::LeftX, 0); // -1..1 (triggers 0..1)
input.rumble(0, 0.4f, 0.8f, 200);           // pad, low-frequency, high-frequency, milliseconds
std::string_view typed = input.text();      // text typed this frame, UTF-8, for name entry and chat
```

Convert the cursor to world coordinates with the camera: `engine.camera().screen_to_world(input.mouse_position())`.

Gamepads are picked up when they're connected, in up to four slots, and released when they're unplugged.

## Rebinding

A "press a key for Jump" screen needs the input the player pushes next:

```cpp
if (auto b = input.last_pressed()) {          // the first key, button or stick pushed this frame
    input.unbind("jump");
    input.bind("jump", *b);
    engine.save_controls();
}
```

`Input::describe(binding)` gives readable names for prompts and settings screens: `"Space"`, `"Left mouse"`,
`"Gamepad a"`, `"Gamepad leftx+"`.

### Saving controls

`engine.save_controls()` stores every action's bindings in the player's settings under `"input"`. At startup, the
engine applies saved controls *after* `Game::init`, so the player's choices override the defaults the game just
bound. The format is readable JSON:

```json
{ "jump": [ { "key": "Space" }, { "button": "a" } ] }
```

`input.save_bindings()` and `input.load_bindings(json)` do the conversion yourself; loading replaces only the actions
it mentions.

## UI actions

The [game UI](ui.md) navigates with the actions `ui_up`, `ui_down`, `ui_left`, `ui_right`, `ui_confirm` and
`ui_back`. If the game doesn't bind them, the UI binds defaults the first time it runs: arrows, WASD, the d-pad and
left stick to move; Enter, Space and South to confirm; Escape and East to go back. Bind them yourself first to change
that.

## The dev overlay

While the F1 overlay has keyboard or mouse focus (typing in a field, dragging a window), the game sees no new
presses from that device, so typing a value doesn't also make the character jump.
