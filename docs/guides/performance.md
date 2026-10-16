# Performance

Most 2D games run comfortably without thinking about performance. When one doesn't, this page says where time
usually goes and what to do about it.

## Measure first

- The overlay's frame graph and **Profiler** panel show where CPU time goes, per zone and per system.
- The **Systems** panel shows each system's time and which systems run in parallel.
- `renderer().stats()` (also in the overlay) shows sprites, draw calls, batches and pipelines per frame.
- Measure `dev` or `release` builds. `debug` builds are slow in different places.

Add `BOWL_PROFILE_SCOPE("name")` around anything you suspect.

## CPU: the simulation

**Run per-entity work in systems with `par_each`.** Work spread over all cores, with no locking in the common case.
Keep the per-entity function free of shared writes.

**Keep queries.** Systems keep their queries for you. If you query by hand every frame, keep the `Query` object
instead of calling `world.each` (which builds a new one each time).

**Don't churn archetypes.** Adding or removing a component moves the entity's data. Doing it to thousands of entities
every tick adds up; a `bool` field or a state enum is cheaper than a tag that comes and goes.

**Use `insert` for several components** instead of several `add` calls: one move instead of many.

**Use a spatial hash** for "what's near me" queries instead of testing every pair
([collision.md](collision.md#many-moving-things-spatialhash)).

**Keep work out of the fixed tick if it isn't simulation.** Animation, particles and camera smoothing belong in
`update` or the frame schedule; the fixed schedule can run several times in one frame when the game catches up.

## CPU: drawing

Filling batches is usually cheap, but:

- **Cull.** `draw_tilemap` culls to the camera for you. For your own sprites over a big world,
  `camera.visible_bounds()` gives the area worth drawing.
- **Keep batches.** A `SpriteBatch` member reuses its memory frame to frame; a local one allocates every frame.
- **Text** caches glyphs, but laying out long text every frame still costs. For static text, consider caching the
  layout size with `measure_text` once.

## GPU

The renderer uploads every sprite of a frame in one go and draws each run of sprites that share a texture with one
call. Draw calls rise with:

- **texture changes** within a batch. Sorting by depth interleaves textures when sprites of different textures
  overlap in depth. Put many small images in one atlas texture (sprite sheets do this), or sort a batch by
  `Texture` when order doesn't matter;
- **batches**: each submitted batch is at least one draw;
- **materials**: each material needs its own pipeline and draws.

Thousands of sprites in a handful of draw calls is normal: the sprites sample draws its 2000 bouncing sprites, five
spinners and text in 5 draw calls.

**Post-processing** costs nothing until turned on. Bloom and lighting each add a few full-screen passes; the light map
is half resolution. On weak GPUs, bloom is the first thing to make optional.

**Overdraw.** Big transparent sprites stacked on each other (fog layers, light pools in the scene rather than the light
map) cost fill rate. Draw lights into the light map, where they're cheaper.

## Memory

- Textures stay in GPU memory while any `Asset` handle refers to them. Drop handles to free them.
- Sounds are fully decoded (48 kHz stereo float: about 23 MB per minute). Use `Music` (streamed) for anything long.
- Font atlas pages are 4 MB each and created as needed; many sizes of many fonts fill pages faster.

## The ECS benchmark

`bowl_bench_ecs` measures entity creation, iteration, parallel iteration and add/remove on a million entities. Run it
from a `release` build when changing the ECS, and compare before and after:

```bash
./build/release/tests/bowl_bench_ecs
```
