# Hot reload

In a dev build, almost everything can change while the game is running: art, sounds, maps, shaders, strings, and the
game's own C++ code. The goal is that you rarely need to restart the game to see a change.

| What | How | Platforms |
| --- | --- | --- |
| Textures, sprite sheets, maps, sounds, music, fonts, string tables | Save the file | All |
| Shaders | Save the `.glsl` file | All |
| Game code | Rebuild the game's code library | macOS, Linux |

## Assets and shaders

Dev builds read assets straight from your source folders and check the files they depend on four times a second.
Save a PNG in your image editor, re-export from Aseprite, save a map in Tiled or a shader in your editor, and the game
picks it up. A file that fails to load (a half-written PNG, a shader typo) logs an error and the previous version stays
in use; fix it and save again.

The details, including how to react to reloads in your own code, are in [assets.md](assets.md#hot-reload) and
[shaders.md](shaders.md#live-editing).

## Game code

With game-code hot reload, your game's code is built as a separate library that the running game loads. Rebuild the
library and the game swaps the new code in, usually in well under a second, keeping the world as it was: every
entity, where it was, with its health, inventory and everything else that's saved.

### Setting it up

Add `HOT_RELOAD` to `ph_add_game`:

```cmake
add_executable(my_game main.cpp)
ph_add_game(my_game ASSETS assets HOT_RELOAD)
```

It needs CMake 3.27 or newer and takes effect only in dev builds on macOS and Linux. Elsewhere (release builds,
Windows, older CMake) the game builds as an ordinary executable, so the same `CMakeLists.txt` works everywhere.

With hot reload on, the build produces two things:

- `my_game`: the engine plus a small host that loads your code;
- `my_game_code`: your game, as a loadable library (`libmy_game_code.so` on Linux and macOS).

### Using it

Run `my_game`, edit your code, save. That's it: the game notices the saved file, rebuilds `my_game_code` in the
background and swaps the new code in. The log shows each step:

```
auto-build: source changed, building
auto-build: built in 2.9 s
hot reload #1: new code running, 1512 entities restored in 216 ms
```

If the code doesn't compile, the compiler's errors appear in the log (and the overlay's **Log** panel) and the game
keeps running the previous code. Fix the error, save, and it carries on.

The game watches the folders that hold the game's source files, and everything below them, for C and C++ files.
Hidden folders and build folders are skipped. Headers that live somewhere else (an `include/` folder next to `src/`)
aren't watched; saving a source file picks up their changes too.

You can still build by hand, from your editor or the command line:

```bash
cmake --build --preset dev --target my_game_code
```

Start the game with `--no-auto-build` to turn automatic builds off, for example while your editor builds on save
itself (two builds of the same target at once can trip over each other).

### Writing reloadable code

A few rules make reloads work well. They're good structure anyway.

**Setup goes in `init`, world creation in `start`.** After a reload, the engine runs the new code's `init()` again,
but not `start()`, because the world is restored from before the reload. So:

- `init`: register systems, bind input, load assets, set resources. It must not create entities.
- `start`: spawn the initial world, push the first scene. It runs once per session.

**Use `PH_COMPONENT` for components.** The world survives a reload by being saved and loaded, so components need to be
savable. `PH_COMPONENT` also registers the type as soon as the library loads, which the restore relies on.

**Rebuild what isn't saved in `init`.** Components that aren't savable, such as an animator holding runtime state,
don't survive. Recreate them in `init` or in a system that adds them where missing. The sprites sample does this for
its banner animations:

```cpp
// in init(): give every Banner an animator if it doesn't have one
engine.frame_schedule().each<const Banner>("animate banners",
    [this](ecs::SystemContext& ctx, ecs::Entity e, const Banner& b) {
        gfx::SpriteAnimator anim;
        anim.sheet = spinner_;
        anim.play("default");
        ctx.commands.add<gfx::SpriteAnimator>(e, anim);
    },
    ecs::without<gfx::SpriteAnimator>());
```

**Keep important state in the world, not in the `Game` object.** The `Game` object is destroyed and created again
on reload, so its member variables start fresh. A score kept in a resource or component survives (if it's savable);
a score kept in `int score_;` on the `Game` doesn't.

### What happens during a reload

1. The host notices the library file changed and waits until the linker has finished writing it.
2. It saves the world.
3. It tears down everything the old code created, *while that code is still loaded* (destructors live in the
   library): scenes, systems, input bindings, the world's entities, hooks and resources, and every asset and loader
   registered since the game started.
4. It loads a fresh copy of the new library. If that fails, the old code is brought back up with the world and the
   game keeps running; the log says why.
5. It creates the new `Game`, runs `init()`, applies the player's saved controls again, and loads the world.

Entity ids change in the process, but references between saved entities are remapped, so a follower still follows
its leader.

### What can't change

Some changes need a restart, because the world's saved form or the engine's state can't absorb them:

- Removing or renaming a component type: entities lose that component (with a warning).
- Changes to `configure()`: the window and engine are already set up.
- Changes to the engine itself: the engine is in the host executable. Rebuild and restart `my_game`.

Adding fields to components is fine: new fields get their default values.

### Debugging

Debuggers handle the library like any other: breakpoints in game code work after a reload (LLDB and GDB pick up the
new library as it loads). The host loads a *copy* of the library file each time, so the build can overwrite the
original while the game runs.

### Windows

Not supported yet. It needs the engine's functions exported from the executable, which on Windows means annotating
the engine's API for `dllexport`. It's on the [roadmap](../roadmap.md). Asset and shader hot reload work on Windows.
