# Saving and loading

Three layers, each built on the one before:

1. **Reflection:** `PH_REFLECT` / `PH_COMPONENT` list a struct's fields.
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
PH_REFLECT(Inventory, items, coins)

struct Health {
    i32 current = 100;
    i32 max = 100;
};
PH_COMPONENT(Health, current, max)   // PH_REFLECT + registration as a component
```

Put the macro after the struct, at namespace scope, in the struct's namespace. Up to 32 fields. Use `PH_COMPONENT`
for components and `PH_REFLECT` for everything else (structs used as fields, settings, save headers).

Reflection gives you, for free:

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

For types that need their own format, define `ph_to_json` and `ph_from_json` next to the type (found by
argument-dependent lookup):

```cpp
struct Version { u8 major, minor; };

json::Value ph_to_json(const Version& v, serial::Context&) {
    return std::format("{}.{}", v.major, v.minor);
}
bool ph_from_json(const json::Value& j, Version& v, serial::Context&) {
    int major = 0, minor = 0;
    if (std::sscanf(std::string(j.as_string()).c_str(), "%d.%d", &major, &minor) != 2) return false;
    v = {static_cast<u8>(major), static_cast<u8>(minor)};
    return true;
}
```

A type with hooks can be a field of reflected structs and a savable component.

## World files

```cpp
#include "ph/ecs/world_io.hpp"

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
  "format": "PLACEHOLDERENGINENAME.world",
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
  be registered before it can load: `PH_COMPONENT` registers it when the program starts. For components without it,
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

Prefabs as hot-reloadable asset files are on the [roadmap](../roadmap.md); for now, load the JSON yourself or wrap it
in a [custom asset type](assets.md#custom-asset-types).

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
PH_REFLECT(VideoSettings, fullscreen, vsync, ui_scale)

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
PH_REFLECT(SaveHeader, location, play_time, day)

json::Value doc;
doc.set("header", serial::to_json(header, ctx));
doc.set("world", ecs::save_world(world, ctx, ecs::without<NotSaved>()));
std::string text = json::stringify(doc);
write_file_bytes(engine.user_data_path() / "slot1.json", text.data(), text.size());
```

Load the header alone to show slot details in a menu without loading the world.
