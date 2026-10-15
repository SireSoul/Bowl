# Assets

Textures, shaders, fonts, sounds, maps, sprite sheets and string tables are *assets*: loaded by a virtual path,
cached, shared, and in dev builds reloaded when their files change.

- [Loading](#loading)
- [Virtual paths and mounts](#virtual-paths-and-mounts)
- [Asset handles](#asset-handles)
- [Built-in asset types](#built-in-asset-types)
- [Assets made in code](#assets-made-in-code)
- [Hot reload](#hot-reload)
- [Custom asset types](#custom-asset-types)
- [Errors](#errors)

## Loading

```cpp
Asset<gfx::Texture> hero = engine.assets().load<gfx::Texture>("characters/hero.png");
Asset<audio::Sound> jump = engine.assets().load<audio::Sound>("sfx/jump.wav");
Asset<gfx::Font> font = engine.assets().load<gfx::Font>("engine/fonts/Roboto-Medium.ttf");
```

`load` returns the cached asset if it's already loaded, so asking twice is cheap and both handles share one copy.
`load` finishes before it returns. Load what a scene needs when it starts (in `init` or `Scene::enter`), or load it
in the background.

The asset manager is main-thread only; don't load from systems running on workers.

### Loading in the background

`load_async` returns the handle at once and does the slow part on a loading thread: reading the file, and for
pictures and sounds, decoding it. What has to happen on the main thread (creating the texture on the GPU) happens
when the App finishes the load, at the start of a frame. The handle is empty until then.

```cpp
// a loading screen
for (const char* path : level_art) art.push_back(engine.assets().load_async<gfx::Texture>(path));
music = engine.assets().load_async<audio::Music>("music/forest.ogg");

// every frame
if (engine.assets().loading() == 0) engine.scenes().replace(...);   // everything's in
draw_progress_bar(1.0f - engine.assets().loading() / f32(total));
```

- Check the handle before using it (`if (art[i])`). Calling `load` for an asset still loading finishes it on the spot,
  so code that needs it right away still gets it.
- Files load one at a time, in the order asked, so the first requested arrive first.
- Every type can load in the background. Types with a *decoder* (textures and sounds) do their decoding on the
  loading thread too; others only read the file there. Your own types can have one: see below.

## Virtual paths and mounts

Paths are virtual: forward slashes, relative, no drive letters. The asset manager resolves them against an ordered
list of *mounts*, each a virtual prefix and a real folder, and the first mount that has the file wins.

The engine sets up these mounts:

| Prefix | Dev builds | All builds |
| --- | --- | --- |
| `engine/` | `<engine source>/engine/assets/` first | `<exe dir>/engine/` |
| (none) | your game's `ASSETS` folder first | `<exe dir>/assets/` |

So `"characters/hero.png"` is `assets/characters/hero.png` in your source tree, and `"engine/fonts/Roboto-Medium.ttf"`
is the engine's font. Because dev builds read the source folders directly, saving a file there is all it takes to
update the game. Release builds read only the copies next to the executable.

Add your own mounts with `assets.mount(prefix, folder)` (for mods or downloadable content, for example);
`assets.resolve(path)` returns the real file a path maps to, if any.

## Asset handles

`Asset<T>` is a shared handle:

```cpp
if (hero) batch.draw(*hero, position);   // false if loading failed
const gfx::Texture* t = hero.get();      // nullptr if not loaded
std::string_view path = hero.path();
u32 v = hero.version();                  // goes up on every reload
```

When an asset reloads, its value is replaced *inside* the shared slot, so every handle sees the new version without
being updated. If you derive something from an asset (a cached layout, a collision grid built from a map), store the
version you built it from and rebuild when `version()` changes:

```cpp
if (map.version() != built_from_) {
    rebuild_collision(*map);
    built_from_ = map.version();
}
```

Handles are cheap to copy (a shared pointer). An `Asset<T>` field in a reflected component saves as its path and
loads again when the world is loaded (see [saving.md](saving.md)).

## Built-in asset types

| Type | Files | Notes |
| --- | --- | --- |
| `gfx::Texture` | `.png` `.jpg` `.tga` `.bmp` | Premultiplied on load; mipmapped unless `pixel_art` is set |
| `gfx::Shader` | `name.vert` / `name.frag` | Path without `.glsl` / `.phsh`; see [shaders.md](shaders.md) |
| `gfx::Font` | `.ttf` `.otf` | See [text.md](text.md) |
| `gfx::SpriteSheet` | Aseprite `.json` | Loads its PNG too; see [sprite-animation.md](sprite-animation.md) |
| `gfx::Tilemap` | Tiled `.tmj` | Loads `.tsj` tilesets and images too; see [tilemaps.md](tilemaps.md) |
| `audio::Sound` | `.wav` `.ogg` | Decoded fully into memory; see [audio.md](audio.md) |
| `audio::Music` | `.ogg` | Streamed while playing |
| `StringTable` | `lang/<code>.json` | Used by localization; see [localization.md](localization.md) |

## Assets made in code

Generated textures, synthesized sounds and procedural maps can be registered too, so they show up in the overlay's
asset list and can be found by path:

```cpp
Asset<gfx::Texture> noise = engine.assets().add("generated/noise", std::move(texture));
Asset<gfx::Texture> again = engine.assets().find<gfx::Texture>("generated/noise");
```

`add` with a path that already exists replaces the value (and bumps the version). Assets made in code are never
reloaded from disk.

## Hot reload

In dev builds the engine checks watched files four times a second. When one changes, every asset that read it is
loaded again:

- a texture when its PNG changes;
- a sprite sheet when its JSON *or* its PNG changes;
- a tilemap when the map, any of its tilesets or any of their images change;
- a shader when its source or any file it `#include`s changes;
- a string table when its JSON changes.

If the new load fails (a half-written file, a shader with a typo, broken JSON), the error is logged and the previous
value stays. Fix the file and save again; there's nothing to restart.

To react to reloads, register a callback:

```cpp
engine.assets().on_reload([](std::string_view path, bool ok) {
    if (ok && path.starts_with("maps/")) { /* rebuild something */ }
});
```

The overlay's **Assets** panel lists every asset with its type and version and has a **reload** button for each.
From code, `assets.reload(path)` does the same. `assets.set_hot_reload(false)` turns watching off.

Hot reload of the game's *code* is a separate feature: see [hot-reload.md](hot-reload.md#game-code).

## Custom asset types

Any type can be an asset. Register a loader: a function that reads files through the `AssetLoadContext` and returns
the value, or `nullptr` on failure.

```cpp
struct Dialogue {
    std::vector<std::string> lines;
};

engine.assets().register_loader<Dialogue>([](AssetLoadContext& ctx) -> std::unique_ptr<Dialogue> {
    auto text = ctx.read_text(ctx.path());
    if (!text) return nullptr;
    auto parsed = json::parse(*text);
    if (!parsed) {
        log_asset_error(ctx.path(), "Dialogue", parsed.error->message);
        return nullptr;
    }
    auto d = std::make_unique<Dialogue>();
    for (const json::Value& line : parsed.value["lines"].array()) d->lines.emplace_back(line.as_string());
    return d;
});

Asset<Dialogue> intro = engine.assets().load<Dialogue>("dialogue/intro.json");
```

Read every file through the context (`ctx.read`, `ctx.read_text`, `ctx.exists`): that's how the asset manager learns
which files the asset depends on, so editing any of them reloads it. A loader that loads *other* assets
(`ctx.assets().load<gfx::Texture>(...)`) gets those reloaded on their own.

Register loaders in `Game::init`, before loading. Registering a loader for a type that already has one replaces it.

For background loads, a type can also have a decoder: a thread-safe function that turns the file's bytes into
something that saves the loader work. The loader finds it in `ctx.prepared<T>()`:

```cpp
engine.assets().register_decoder<Dialogue>([](std::string_view, std::span<const u8> bytes) -> std::shared_ptr<void> {
    return std::make_shared<json::ParseResult>(json::parse({reinterpret_cast<const char*>(bytes.data()), bytes.size()}));
});
// in the loader:
if (auto* parsed = ctx.prepared<json::ParseResult>()) { /* use it instead of reading and parsing */ }
```

A plain `load` never calls the decoder, so the loader must still work without it.

## Errors

Failed loads never crash the game. The handle is empty, the log says which file and why, and the overlay's asset list
shows the asset as not loaded. If the file appears or gets fixed later, hot reload fills the handle in. Check handles
before using them (`if (texture) ...`) wherever a missing asset is possible.
