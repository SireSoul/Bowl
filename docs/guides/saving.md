# Saving and loading

Three layers, each built on the one before:

1. **Reflection:** `BOWL_REFLECT` / `BOWL_COMPONENT` list a struct's fields.
2. **Serialization:** reflected structs (and most standard types) convert to and from JSON.
3. **World files:** whole worlds, or chosen entities, save and load as JSON, with references between entities kept
   intact. Prefabs use the same format.

Player preferences are separate and simpler: [player settings](#player-settings).

## Reflection

```cpp
struct Inventory {
    std::vector<std::string> items;
    i32 coins = 0;
};
BOWL_REFLECT(Inventory, items, coins)

struct Health {
    i32 current = 100;
    i32 max = 100;
};
BOWL_COMPONENT(Health, current, max)   // BOWL_REFLECT + registration as a component
```

Put the macro after the struct, at namespace scope, in the struct's namespace. Up to 32 fields. Use `BOWL_COMPONENT`
for components and `BOWL_REFLECT` for everything else (structs used as fields, settings, save headers).

With reflection you get:

- JSON conversion (`serial::to_json`, `serial::from_json`);
- world saving and loading for components;
- live editing in the F1 inspector;
- survival across [game-code hot reload](hot-reload.md#game-code).

You can also walk the fields yourself:

```cpp
reflect::for_each_field(health, [](std::string_view name, auto& value) { /* ... */ });
```

## Serialization

```cpp
serial::Context ctx{&engine.assets()};
json::Value j = serial::to_json(inventory, ctx);
std::string text = json::stringify(j);              // pretty-printed; json::stringify(j, -1) for one line

Inventory loaded;
bool ok = serial::from_json(j, loaded, ctx);        // false if something had the wrong type
for (const std::string& w : ctx.warnings) warn("{}", w);
```

What can be serialized:

| Type | JSON |
| --- | --- |
| `bool`, integers, floats | boolean, number |
| enums | their number |
| `std::string` | string |
| `Vec2`, `Vec3`, `Vec4`, `Color`, `Rect` | array of numbers |
| `std::vector<T>`, `std::array<T, N>` | array |
| `std::optional<T>` | the value, or `null` |
| `Asset<T>` | the asset's path; loading loads the asset (needs `ctx.assets`) |
| `ecs::Entity` | a file-local id (see below) |
| reflected structs | object with one key per field |
| anything with hooks | whatever the hooks write |

Trying to save anything else is a compile error that says to reflect the type or add hooks.

### Loading is forgiving

- A field missing from the JSON keeps its current (default) value. Adding fields to a struct never breaks old files.
- A field with the wrong type keeps its value, adds a warning to `ctx.warnings`, and makes `from_json` return
  `false`; the other fields still load.
- Unknown keys are ignored. Removing a field never breaks old files either.

Renaming a field is the one change that loses data: the old key is ignored and the new field keeps its default. If
you must rename, keep reading the old name with hooks for a while.

### Custom conversion: hooks

For types that need their own format, define `bowl_to_json` and `bowl_from_json` next to the type (found by
argument-dependent lookup):

```cpp
struct Version { u8 major, minor; };

json::Value bowl_to_json(const Version& v, serial::Context&) {
    return std::format("{}.{}", v.major, v.minor);
}
bool bowl_from_json(const json::Value& j, Version& v, serial::Context&) {
    int major = 0, minor = 0;
    if (std::sscanf(std::string(j.as_string()).c_str(), "%d.%d", &major, &minor) != 2) return false;
    v = {static_cast<u8>(major), static_cast<u8>(minor)};
    return true;
}
```

A type with hooks can be a field of reflected structs and a savable component.

## World files

```cpp
#include "bowl/ecs/world_io.hpp"

auto file = engine.user_data_path() / "slot1.json";
serial::Context ctx{&engine.assets()};

// save
std::string text = json::stringify(ecs::save_world(engine.world(), ctx));
write_file_bytes(file, text.data(), text.size());

// load
if (auto saved = read_file_text(file)) {
    if (auto parsed = json::parse(*saved)) {
        std::vector<ecs::Entity> spawned = ecs::load_entities(engine.world(), parsed.value, ctx);
    }
}
for (const std::string& w : ctx.warnings) warn("load: {}", w);
```

`save_world` writes every entity that has at least one savable component; pass a filter to choose, such as
`ecs::without<Transient>()`. `save_entities(world, entities, ctx)` saves a specific list. `load_entities` *adds* the
saved entities to the world and returns them in file order; clear the world first if you're replacing it.

The format:

```json
{
  "format": "Bowl.world",
  "version": 1,
  "entities": [
    { "id": 0, "components": { "Health": { "current": 80, "max": 100 }, "Player": {} } },
    { "id": 1, "components": { "Follower": { "leader": 0 } } }
  ]
}
```

- **Savable components** are reflected ones, ones with hooks, and empty tags. Others are skipped (one warning per
  type), so runtime-only components (caches, GPU handles, animator state) aren't saved.
- **Components are found by type name** when loading, as written by the compiler (including namespaces). A type must
  be registered before it can load: `BOWL_COMPONENT` registers it when the program starts. For components without it,
  call `ecs::register_components<A, B>()` before loading. Unknown names are skipped with a warning.
- **Entity references** (`ecs::Entity` fields, including inside vectors and nested structs) are written as file ids
  and turned back into the new entities' ids when loading, so they stay correct even when loading into a world that
  already has entities. A reference to an entity that wasn't saved becomes null.
- **Assets** in components are saved by path and loaded again.

Renaming a component type or moving it to another namespace changes its saved name. Old files will skip it with a
warning.

### Prefabs

A prefab is one entity object from the format above (`{"components": {...}}`). Write them by hand, take one from
the inspector (**Copy JSON** copies a world document; its single entry under `"entities"` is the prefab), or save
them from code; then spawn as many as you need:

```json
{ "components": { "Health": { "max": 40, "current": 40 }, "Enemy": {}, "Sprite": { "texture": "enemies/slime.png" } } }
```

```cpp
ecs::Entity slime = ecs::spawn_prefab(engine.world(), prefab_json, ctx);
world.get<Position>(slime).value = spawn_point;
```

#### Prefab files

Saved as `.prefab` files, prefabs are assets that hot-reload like any other:

```cpp
Asset<ecs::Prefab> slime = engine.assets().load<ecs::Prefab>("prefabs/slime.prefab");

std::vector<ecs::Entity> spawned = ecs::instantiate(engine.world(), *slime, ctx);   // spawned[0] is the root
```

A prefab file holds one entity (`{"components": {...}}`, as above) or a whole world document with several entities
that refer to each other, such as a cart and its wheels, or a door and its switch: save them with
`ecs::save_entities` and write the result to a `.prefab` file. Each `instantiate` makes fresh copies with the
references pointing at the new entities. Editing the file changes the copies spawned after it; ones already in the
world stay as they are.

### From the overlay

The F1 overlay's **Entities** panel has **Save world** and **Load (adds to world)**, which use `devtools_world.json`
in the working directory. Handy for capturing a situation and coming back to it.

## Player settings

`engine.settings()` holds preferences that outlive a play session: volume, language, window mode, controls, a high
score. They're stored as JSON in the operating system's per-user folder (see
[game-loop.md](game-loop.md#where-files-go)), loaded at startup and saved at exit if anything changed.

```cpp
f32 music = engine.settings().get("audio.music", 0.8f);   // the fallback is used when missing
engine.settings().set("audio.music", music);
bool fullscreen = engine.settings().get("video.fullscreen", false);
engine.settings().save();                                 // optional: write now (e.g. when leaving an options menu)
```

Dotted keys address nested objects (`{"audio": {"music": 0.8}}`). Values go through the serializer, so reflected
structs work too:

```cpp
struct VideoSettings { bool fullscreen = false; bool vsync = true; f32 ui_scale = 1; };
BOWL_REFLECT(VideoSettings, fullscreen, vsync, ui_scale)

auto video = engine.settings().get("video", VideoSettings{});
```

Saving writes to a temporary file and renames it over the old one, so a crash never leaves a half-written settings
file. A settings file that can't be parsed is ignored (with a warning) and the game starts from defaults.

Controls are stored under `"input"` by `engine.save_controls()`; see [input.md](input.md#saving-controls).

## Save games: putting it together

A typical save slot is a small header plus the world:

```cpp
struct SaveHeader {
    std::string location;
    f64 play_time = 0;
    i32 day = 1;
};
BOWL_REFLECT(SaveHeader, location, play_time, day)

json::Value doc;
doc.set("header", serial::to_json(header, ctx));
doc.set("world", ecs::save_world(world, ctx, ecs::without<NotSaved>()));
std::string text = json::stringify(doc);
write_file_bytes(engine.user_data_path() / "slot1.json", text.data(), text.size());
```

Load the header alone to show slot details in a menu without loading the world.

## Big worlds: the binary form

Text is easy to read and diff, but a world with tens of thousands of entities makes a big file that takes a while to
parse. `json::encode_binary` writes the same document in a compact binary form: every key and string is stored once
and numbers take only the bytes they need. A world of many similar entities comes out several times smaller, and reading
it skips text parsing.

```cpp
std::vector<u8> bytes = json::encode_binary(ecs::save_world(world, ctx));
write_file_bytes(engine.user_data_path() / "slot1.save", bytes.data(), bytes.size());

if (auto bytes = read_file_bytes(engine.user_data_path() / "slot1.save"))
    if (auto doc = json::parse_any(*bytes))       // text or binary
        ecs::load_entities(world, doc.value, ctx);
```

`json::parse_any` reads either form, and so do the `WorldFile` and `Prefab` loaders and Bowl Editor (which saves a
binary file back as binary). The binary form decodes back to exactly the same document. A damaged or truncated file
fails to load with an error; it never crashes.

## Changing the save format: migrations

Games change after release: a component gets renamed, a field splits in two. Give your saves a version number and
write one small step per change, so old saves still load:

```cpp
constexpr i32 save_version = 3;

ecs::Migrations migrations;
migrations.add(0, [](json::Value& doc) { ecs::rename_component(doc, "game::Hp", "game::Health"); });
migrations.add(1, [](json::Value& doc) { ecs::rename_field(doc, "game::Health", "hp", "current"); });
migrations.add(2, [](json::Value& doc) {
    ecs::for_each_saved(doc, "game::Health", [](json::Value& health) {
        health.set("max", health["current"].as_int() * 2);   // a new field worked out from old data
    });
});

// saving
serial::Context ctx{.assets = &engine.assets(), .game_version = save_version};
json::Value doc = ecs::save_world(world, ctx);         // writes "game_version": 3

// loading
if (!migrations.upgrade(doc, ctx)) { /* newer than this game, or a step is missing: see ctx.warnings */ }
ecs::load_entities(world, doc, ctx);
```

`add(n, step)` turns a version `n` document into version `n + 1`. `upgrade` runs every step from the file's
`game_version` (0 if the file has none) up to `latest()` and records the new version, so upgrading twice is
harmless. A file from a newer version of the game is refused rather than half-loaded.

The helpers work on every saved entity: `rename_component`, `remove_component`, `rename_field`, and
`for_each_saved` for anything else. Documents are plain `json::Value`s, so a step can also change them directly
(`find`, `set`, `erase`).
