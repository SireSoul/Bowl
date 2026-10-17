# ecs

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/ecs/archetype.hpp](#bowlecsarchetypehpp)
- [bowl/ecs/commands.hpp](#bowlecscommandshpp)
- [bowl/ecs/component.hpp](#bowlecscomponenthpp)
- [bowl/ecs/ecs.hpp](#bowlecsecshpp)
- [bowl/ecs/hierarchy.hpp](#bowlecshierarchyhpp)
- [bowl/ecs/schedule.hpp](#bowlecsschedulehpp)
- [bowl/ecs/world.hpp](#bowlecsworldhpp)
- [bowl/ecs/world_io.hpp](#bowlecsworld_iohpp)

## bowl/ecs/archetype.hpp

`#include "bowl/ecs/archetype.hpp"` · namespace `bowl::ecs`

### Archetype

All entities with exactly the same set of components live in one archetype, each component type in its own tightly
packed array (structure of arrays), so a query walks memory front to back.

```cpp
class Archetype
```

```cpp
Archetype(u32 id, std::vector<ComponentId> components);
~Archetype();
Archetype(const Archetype&) = delete;
Archetype& operator=(const Archetype&) = delete;
```

```cpp
u32 id() const;
const ComponentMask& mask() const;
std::span<const ComponentId> components() const;
usize size() const;
bool empty() const;
std::span<const Entity> entities() const;
bool has(ComponentId c) const;
```

Base pointer of a component column (nullptr for tags, or if the archetype lacks the component).

```cpp
void* column(ComponentId c) const;
void* at(ComponentId c, u32 row) const;
```

The rest is for World.

Appends a row whose components are uninitialized; the caller constructs them.

```cpp
u32 push(Entity e);
```

Destroys the row's components and fills the hole with the last row. Returns the entity that moved into `row`
(null if `row` was the last row).

```cpp
Entity remove(u32 row);
```

Moves the row to `dst`, carrying over the components both archetypes share. Components only `dst` has are left
uninitialized for the caller. Returns the new row in `dst`; `moved` receives the entity that took the old row.

```cpp
u32 move_to(u32 row, Archetype& dst, Entity& moved);
```

```cpp
std::unordered_map<ComponentId, Archetype*> add_edges;
std::unordered_map<ComponentId, Archetype*> remove_edges;
```

## bowl/ecs/commands.hpp

`#include "bowl/ecs/commands.hpp"` · namespace `bowl::ecs`

### CommandBuffer

Records structural changes (create, destroy, add, remove) to apply later, in recording order. Systems use these
while iterating, because changing an archetype mid-iteration would invalidate it. Payloads are placed in a bump
arena, so recording is allocation-free in the steady state.

```cpp
class CommandBuffer
```

```cpp
explicit CommandBuffer(World& world);
~CommandBuffer();
CommandBuffer(CommandBuffer&&) noexcept;
CommandBuffer& operator=(CommandBuffer&&) noexcept;
CommandBuffer(const CommandBuffer&) = delete;
CommandBuffer& operator=(const CommandBuffer&) = delete;
```

Reserves an entity id now (thread-safe); the entity becomes alive when the buffer is applied.

A system's buffer (and each chunk's in a par_each) hands out a stand-in id instead (Entity::is_placeholder),
because systems running side by side would otherwise race for ids and get different ones each run. Stand-ins
become real entities when the commands are applied, in system and chunk order, so a simulation creates the same
ids every time. Use a stand-in in this buffer's commands and in entity fields of reflected components (both are
swapped for the real id); don't keep it beyond that.

```cpp
Entity create();
template <class... Ts>
    requires(Component<std::decay_t<Ts>> && ...)
Entity create(Ts&&... components);
void destroy(Entity e);
template <Component T, class... Args> void add(Entity e, Args&&... args);
template <class... Ts>
    requires(Component<std::decay_t<Ts>> && ...)
void insert(Entity e, Ts&&... components);
template <Component T> void remove(Entity e);
```

Arbitrary deferred work, run in order with the other commands.

```cpp
void run(std::function<void(World&)> fn);
```

Applies every command to the world in recording order, then clears. Commands targeting entities that died in
the meantime are skipped.

```cpp
void apply();
```

Drops the commands unapplied. Entity ids create() reserved go back to the world (their handles stay dead), so a
buffer thrown away doesn't use up slots for good. (The destructor does the same: a buffer mustn't outlive its
world.)

```cpp
void clear();
bool empty() const;
usize size() const;
```

Moves all of `other`'s commands to the end of this buffer.

```cpp
void append(CommandBuffer&& other);
```

Makes create() hand out stand-in ids marked with `tag` (1 to 4095, unique among the buffers applied together).
Schedules do this for their systems.

```cpp
void use_placeholders(u32 tag);
```

## bowl/ecs/component.hpp

`#include "bowl/ecs/component.hpp"` · namespace `bowl::ecs`

```cpp
#define BOWL_ECS_MAX_COMPONENTS 256
```

### Entity

An entity is an index plus a generation. Destroying an entity bumps its slot's generation, so stale handles are
detected instead of silently aliasing a new entity. Generation 0 is never used: Entity{} is the null entity.

```cpp
struct Entity
```

```cpp
u32 index = 0;
u32 generation = 0;
```

Systems' command buffers hand out stand-in ids with this generation (never a real one); they become real
entities, in a fixed order, when the commands are applied. See CommandBuffer::create.

```cpp
static constexpr u32 placeholder_generation = 0xFFFFFFFFu;
```

```cpp
constexpr bool is_null() const;
constexpr bool is_placeholder() const;
constexpr explicit operator bool() const;
constexpr u64 bits() const;
static constexpr Entity from_bits(u64 b);
constexpr bool operator==(const Entity&) const = default;
```

```cpp
inline constexpr Entity null_entity{};
```

Entities in saved data are written as ids local to the file and remapped on load (ctx.entity_map), so references
between saved entities survive. A reference to an entity that wasn't saved becomes null.

```cpp
inline json::Value bowl_to_json(const Entity& e, serial::Context& ctx);
inline bool bowl_from_json(const json::Value& j, Entity& e, serial::Context& ctx);
inline void bowl_visit(reflect::FieldVisitor& v, std::string_view name, Entity& e);
```

```cpp
using ComponentId = u16;
inline constexpr usize max_components = BOWL_ECS_MAX_COMPONENTS;
```

A component is any move-constructible, destructible struct; no base class or generated code. Empty structs are
tags and take no memory. Adding BOWL_COMPONENT(Type, fields...) after one makes it savable and editable in the
inspector, but is optional.

```cpp
template <class T>
concept Component = std::is_object_v<T> && !std::is_const_v<T> && std::is_move_constructible_v<T> &&
                    std::is_destructible_v<T>;
```

### ComponentInfo

```cpp
struct ComponentInfo
```

```cpp
std::string_view name;
u32 size = 0; // 0 for tags
u32 align = 1;
bool trivially_relocatable = false;
void (*move_construct)(void* dst, void* src) = nullptr; // placement-move src into uninitialized dst
void (*destroy)(void* ptr) = nullptr;
```

Present for savable components: reflected (BOWL_REFLECT), with serialization hooks, or tags.

```cpp
void (*construct_default)(void* ptr) = nullptr;
json::Value (*save)(const void* ptr, serial::Context& ctx) = nullptr;
bool (*load)(const json::Value& j, void* ptr, serial::Context& ctx) = nullptr;
void (*visit)(void* ptr, reflect::FieldVisitor& v) = nullptr; // live editing (reflected only)
```

Who registered it: the type's anchor and its module's (type_id.hpp). Two types can share a name (ones in
anonymous namespaces or functions); these tell them apart.

```cpp
const void* type_key = nullptr;
const void* module_key = nullptr;
```

Its code was in a game library that hot reload unloaded; the next registration of the name refreshes it.

```cpp
bool dormant = false;
bool savable() const;
```

```cpp
template <class T>
concept SavableComponent = std::is_default_constructible_v<T> &&
                           (std::is_empty_v<T> || reflect::Reflected<T> || serial::HasHooks<T>);
```

```cpp
const ComponentInfo& component_info(ComponentId id);
usize component_type_count();
```

Looks a component type up by its type_name (e.g. "game::Health"). Only types used (or registered) so far exist.
Types that share a name (in anonymous namespaces in different files, say) are told apart by a number: the first
one registered has the plain name, the next "Name #2", and so on.

```cpp
std::optional<ComponentId> find_component(std::string_view name);
```

const T and T share one id (const only expresses read-only access in queries).

```cpp
template <class T> ComponentId component_id();
```

Makes component types known by name before anything uses them, so saved worlds that mention them can load.

```cpp
template <class... Ts> void register_components();
```

Hot reload: marks registrations whose code satisfies `owned` (lives in the library being unloaded) as dormant, and
returns what they were so restore_components can undo it if the new library fails to load.

```cpp
std::vector<std::pair<ComponentId, ComponentInfo>> mark_components_dormant(bool (*owned)(const ComponentInfo&, void*),
                                                                           void* user);
void restore_components(const std::vector<std::pair<ComponentId, ComponentInfo>>& saved);
```

BOWL_COMPONENT(Type, fields...): BOWL_REFLECT plus registration when the program (or game library) loads, so saved
worlds can name the type before any code touches it. Use it for components; BOWL_REFLECT for other structs.
The registration is a static in every file that includes the definition, not an inline variable: compilers may skip
initializing an inline variable nothing uses, and GCC does, which left types unknown by name on Linux.

```cpp
#define BOWL_COMPONENT(Type, ...)
```

### ComponentMask

Fixed-size bitset over component ids.

```cpp
struct ComponentMask
```

```cpp
std::array<u64, max_components / 64> words{};
```

```cpp
void set(ComponentId id);
void reset(ComponentId id);
bool test(ComponentId id) const;
bool contains(const ComponentMask& o) const;
bool intersects(const ComponentMask& o) const;
bool empty() const;
ComponentMask operator|(const ComponentMask& o) const;
ComponentMask operator&(const ComponentMask& o) const;
bool operator==(const ComponentMask&) const = default;
usize hash() const;
template <class F> void for_each(F&& f) const;
```

```cpp
template <class... Ts> static ComponentMask of();
```

### ComponentMaskHash

```cpp
struct ComponentMaskHash
```

```cpp
usize operator()(const ComponentMask& m) const;
```

### std

```cpp
template <> struct std::hash<bowl::ecs::Entity>
```

```cpp
std::size_t operator()(const bowl::ecs::Entity& e) const noexcept;
```

## bowl/ecs/ecs.hpp

`#include "bowl/ecs/ecs.hpp"`

## bowl/ecs/hierarchy.hpp

`#include "bowl/ecs/hierarchy.hpp"` · namespace `bowl::ecs`

Parent/child transforms. Give an entity a Transform2D (relative to its parent) and the engine keeps a
GlobalTransform2D (in the world) up to date once the transform systems run:

```cpp
Entity ship = world.create(Transform2D{{100, 100}});
Entity turret = world.create(Transform2D{{0, -12}});
set_parent(world, turret, ship);   // the turret now moves and rotates with the ship
add_transform_systems(engine.frame_schedule());
```
All of these components are reflected: hierarchies save, load and show in the inspector.

### Transform2D

```cpp
struct Transform2D {
    Vec2 position;
    f32 rotation = 0; // radians
    Vec2 scale{1, 1};
};
```

```cpp
BOWL_COMPONENT(Transform2D, position, rotation, scale)
```

### GlobalTransform2D

Written by propagate_transforms; read it when drawing.

```cpp
struct GlobalTransform2D
```

```cpp
Vec2 position;
f32 rotation = 0;
Vec2 scale{1, 1};
```

```cpp
Vec2 apply(Vec2 local) const;
```

```cpp
BOWL_COMPONENT(GlobalTransform2D, position, rotation, scale)
```

### Parent

```cpp
struct Parent {
    Entity entity;
};
```

```cpp
BOWL_COMPONENT(Parent, entity)
```

### Children

```cpp
struct Children {
    std::vector<Entity> list;
};
```

```cpp
BOWL_COMPONENT(Children, list)
```

### Name

A readable name, shown by the editor and the overlay instead of the entity's id.

```cpp
struct Name {
    std::string value;
};
```

```cpp
BOWL_COMPONENT(Name, value)
```

Attaches child under parent (null parent: detach). Refuses (returns false) if that would make a cycle.
Not while iterating; from systems use ctx.commands.run([=](World& w) { set_parent(w, child, parent); }).

```cpp
bool set_parent(World& world, Entity child, Entity parent);
```

Destroys an entity and everything below it.

```cpp
void destroy_recursive(World& world, Entity root);
```

Computes GlobalTransform2D for every entity with a Transform2D (adding it where missing), parents before children.

```cpp
void propagate_transforms(World& world);
```

Registers propagate_transforms as an exclusive system at the end of the schedule.

```cpp
void add_transform_systems(Schedule& schedule);
```

## bowl/ecs/schedule.hpp

`#include "bowl/ecs/schedule.hpp"` · namespace `bowl::ecs`

### SystemContext

```cpp
struct SystemContext {
    World& world;
    CommandBuffer& commands;
    JobSystem& jobs;
    f32 dt;
};
```

### SystemInfo

What a schedule's system does and where it runs, for tools (the F1 overlay, Bowl Editor's Game tab).

```cpp
struct SystemInfo {
    std::string name;
    u32 batch = 0;     // systems in one batch run side by side
    f64 ms = 0;        // its last run
    bool enabled = true;
    bool exclusive = false; // declared no access: runs alone
    std::vector<std::string> reads, writes;                   // component names
    std::vector<std::string> resource_reads, resource_writes; // resource type names
    // Why it starts a new batch instead of joining the one before: "barrier", "exclusive", or the system it conflicts
    // with and on what ("writes Position, which physics reads"). Empty for the first system and those that joined.
    std::string why_new_batch;
};
```

```cpp
BOWL_REFLECT(SystemInfo, name, batch, ms, enabled, exclusive, reads, writes, resource_reads, resource_writes,
             why_new_batch)
```

### Schedule

An ordered list of systems. Systems that declare non-conflicting access run in parallel; the result is always the
same as running them one by one in insertion order, because only systems with no data in common share a batch.

```cpp
schedule.par_each<Position, const Velocity>("integrate", [](Position& p, const Velocity& v) { ... });
schedule.add("spawner", [](SystemContext& ctx) { ... });               // exclusive: runs alone
schedule.add("ai", fn).reads<Position>().writes<Brain>();              // declared access: may run in parallel
```
Structural changes go through ctx.commands and are applied in system order at barrier() points and at the end of
run(), so thread timing never changes the result.

```cpp
class Schedule : NonCopyable
```

#### System

```cpp
struct System
```

```cpp
std::string name;
std::function<void(SystemContext&)> fn;
ComponentMask reads, writes;
std::vector<TypeId> resource_reads, resource_writes;
std::vector<std::string> resource_read_names, resource_write_names; // owned: a reloaded library's text goes
std::string why_new_batch; // see SystemInfo
bool exclusive = true;
bool barrier_after = false;
bool enabled = true;
f64 last_ms = 0;
std::unique_ptr<CommandBuffer> commands;
```

#### Builder

```cpp
class Builder
```

```cpp
Builder(Schedule& s, usize index);
template <class... Ts> Builder& reads();
template <class... Ts> Builder& writes();
template <class... Rs> Builder& reads_resource();
template <class... Rs> Builder& writes_resource();
```

Run alone, after everything before it and before everything after it.

```cpp
Builder& exclusive();
```

```cpp
Schedule() = default;
```

A system that may do anything; exclusive unless access is declared on the returned builder.

```cpp
Builder add(std::string name, std::function<void(SystemContext&)> fn);
```

A system over every entity matching Ts (const T = read-only). f(Ts&...), f(Entity, Ts&...) or
f(SystemContext&, Entity, Ts&...). Access is deduced from Ts.

```cpp
template <class... Ts, class F> Builder each(std::string name, F f, QueryFilter filter = {});
```

As each(), with the entities split across worker threads. Every chunk gets its own command buffer, merged in
chunk order, so commands stay deterministic.

```cpp
template <class... Ts, class F> Builder par_each(std::string name, F f, QueryFilter filter = {}, usize min_chunk = 512);
```

Applies every command recorded so far before later systems run.

```cpp
void barrier();
```

Removes every system.

```cpp
void clear();
```

```cpp
void run(World& world, JobSystem& jobs, f32 dt);
```

Run everything on the calling thread, in order (to debug a suspected race).

```cpp
void set_parallel(bool parallel);
bool parallel() const;
void set_enabled(std::string_view name, bool enabled);
```

```cpp
const std::vector<std::unique_ptr<System>>& systems() const;
```

Batches of system indices that run concurrently (for tools).

```cpp
const std::vector<std::vector<usize>>& batches();
```

Every system in order, with its access and batch (for tools).

```cpp
std::vector<SystemInfo> describe();
```

Why a and b can't run side by side, as a sentence ("" when they can): "physics is exclusive", "ai writes Brain,
which steering reads".

```cpp
std::string conflict_reason(const System& a, const System& b) const;
```

## bowl/ecs/world.hpp

`#include "bowl/ecs/world.hpp"` · namespace `bowl::ecs`

### QueryFilter

Extra include/exclude terms for a query, for components you filter on but don't read.

```cpp
struct QueryFilter
```

```cpp
ComponentMask with, without;
template <class... Ts> QueryFilter& require();
template <class... Ts> QueryFilter& exclude();
```

```cpp
template <class... Ts> QueryFilter with();
template <class... Ts> QueryFilter without();
```

```cpp
using Hook = std::function<void(World&, Entity)>;
```

### World

Owns entities, their components, and singleton resources.

```cpp
World world;
Entity e = world.create(Position{0, 0}, Velocity{1, 0});
world.each<Position, const Velocity>([](Position& p, const Velocity& v) { p.value += v.value; });
```
Structural changes (create/destroy/add/remove) are not allowed while a query is iterating; record them in a
CommandBuffer (world.deferred(), or the one systems receive) instead. Reading and writing component values of
other entities with get()/try_get() is always fine.

```cpp
class World : NonCopyable
```

```cpp
World();
~World();
```

Entities

```cpp
Entity create();
template <class... Ts>
    requires(sizeof...(Ts) > 0 && (Component<std::decay_t<Ts>> && ...))
Entity create(Ts&&... components);
void destroy(Entity e);
bool alive(Entity e) const;
usize entity_count() const;
```

**Components**

Adds T (constructed from args) or replaces the existing one. Returns the stored component.

```cpp
template <Component T, class... Args> T& add(Entity e, Args&&... args);
```

Adds or replaces several components with a single archetype move.

```cpp
template <class... Ts>
    requires(Component<std::decay_t<Ts>> && ...)
void insert(Entity e, Ts&&... components);
template <Component T> void remove(Entity e);
template <class T> bool has(Entity e) const;
template <class T> T* try_get(Entity e) const;
template <class T> T& get(Entity e) const;
```

Component ids of an entity, for tools (see component_info(id).name).

```cpp
std::span<const ComponentId> components_of(Entity e) const;
void* component_ptr(Entity e, ComponentId c) const;
```

Adds a component by id, for loaders and tools that only know the type at runtime. It is default-constructed,
then `init` fills it in (init gets nullptr for tags). Replaces an existing one. Returns false if the type isn't
savable (see SavableComponent), since only those can be default-constructed this way.

```cpp
bool add_component(Entity e, ComponentId c, FunctionRef<void(void*)> init);
```

Removes a component by id. Returns false if the entity is dead or doesn't have it.

```cpp
bool remove_component(Entity e, ComponentId c);
```

**Queries**

Ts may be const-qualified to declare read-only access. Keep the Query around (in a system, say): it caches the
matching archetypes and picks up new ones as they appear.

```cpp
template <class... Ts> Query<Ts...> query(QueryFilter filter = {});
```

One-shot convenience: f(Ts&...) or f(Entity, Ts&...).

```cpp
template <class... Ts, class F> void each(F&& f);
```

**Hooks**

on_add runs after T is added to an entity; on_remove runs before T is removed or its entity destroyed (the
component is still readable). Hooks run with the world locked; use deferred() for structural changes.

```cpp
template <Component T> void on_add(Hook hook);
template <Component T> void on_remove(Hook hook);
```

Resources: one value per type, for things like time, input, renderer handles and game state

```cpp
template <class T, class... Args> T& set_resource(Args&&... args);
template <class T> T* try_resource() const;
template <class T> T& resource() const;
template <class T> void remove_resource();
```

**Deferred changes**

```cpp
CommandBuffer& deferred();
```

Applies deferred() commands. The App calls this between schedules.

```cpp
void flush();
```

Back to a brand-new world: entities, archetypes, hooks and resources all go (no hooks run). Game-code hot
reload calls this at shutdown, while the game library is still loaded, since its destructors live there.

```cpp
void reset();
```

Drops every hook, resource and pending deferred command, keeping entities and components. Hot reload uses it to
let go of game code while the world itself stays.

```cpp
void forget_code();
```

While a command buffer is being applied: the real entity for one of its stand-in ids (made on first use). Null
outside of that. Other ids come back unchanged.

```cpp
Entity resolve_placeholder(Entity e);
bool locked() const;
```

**Introspection**

```cpp
std::span<const std::unique_ptr<Archetype>> archetypes() const;
```

Bumped whenever a new archetype appears; queries use it to refresh their caches.

```cpp
usize archetype_generation() const;
```

Bumped by compact(), which removes archetypes: queries start their caches over when it changes.

```cpp
u64 layout_epoch() const;
```

Frees the archetypes (component combinations) no entity uses any more. Each one costs a little memory and a
little time in queries, so a world that goes through many combinations (between levels, after a big cleanup)
can call this now and then. Returns how many were freed.

```cpp
usize compact();
```

#### LockGuard

```cpp
struct LockGuard
```

```cpp
World& w;
explicit LockGuard(World& world);
~LockGuard();
LockGuard(const LockGuard&) = delete;
LockGuard& operator=(const LockGuard&) = delete;
```

**Query**

### Query

```cpp
template <class... Ts> class Query
```

```cpp
explicit Query(World& world, QueryFilter filter = {});
```

f(Ts&...) or f(Entity, Ts&...).

```cpp
template <class F> void each(F&& f);
```

Like each, split across worker threads. f must only touch its own entity's components (plus read-only data).

```cpp
template <class F> void par_each(JobSystem& jobs, F&& f, usize min_chunk = 256);
```

f(chunk_index, Entity, Ts&...). Chunks depend only on the entity count and min_chunk, so per-chunk outputs
(e.g. command buffers) can be merged in chunk order for deterministic results.

```cpp
template <class F> void par_each_chunked(JobSystem& jobs, usize min_chunk, F&& f);
```

Raw column access per archetype for hand-vectorized loops: f(std::span&lt;const Entity>, Ts*...).

```cpp
template <class F> void each_archetype(F&& f);
```

```cpp
usize count();
```

The first matching entity, or null.

```cpp
Entity first();
```

```cpp
const ComponentMask& includes() const;
const ComponentMask& excludes() const;
static ComponentMask reads();
static ComponentMask writes();
```

**World templates**

```cpp
template <class... Ts>
    requires(Component<std::decay_t<Ts>> && ...)
void World::insert(Entity e, Ts&&... components);
```

**CommandBuffer templates**

## bowl/ecs/world_io.hpp

`#include "bowl/ecs/world_io.hpp"` · namespace `bowl::ecs`

Saving and loading entities as JSON, for save games, scenes, prefabs and copy/paste in tools.

```cpp
{ "format": "Bowl.world", "version": 1,
  "entities": [ { "id": 0, "components": { "game::Health": { "current": 80, "max": 100 }, "game::Player": {} } } ] }
```
Components are saved by type name when they're savable: reflected with BOWL_REFLECT, given
bowl_to_json/bowl_from_json hooks, or empty tags. Others are skipped (once-per-type warning in ctx.warnings). Entity fields that point at other
saved entities are rewritten to file ids and back, so references survive a round trip.

Documents can be written as text (json::stringify) or, for big worlds, binary (json::encode_binary); everything that
reads world files takes either.

```cpp
inline constexpr std::string_view world_format = "Bowl.world";
inline constexpr int world_format_version = 1;
```

Saves the given entities (dead ones are skipped).

```cpp
json::Value save_entities(const World& world, std::span<const Entity> entities, serial::Context& ctx);
```

Saves every entity that has at least one savable component and passes the filter.

```cpp
json::Value save_world(const World& world, serial::Context& ctx, const QueryFilter& filter = {});
```

Spawns the saved entities into the world (adding to what's there) and returns them in file order.
Unknown component names are skipped with a warning. Types the program hasn't used yet are unknown until
registered: use BOWL_COMPONENT, or call register_components&lt;...>() before loading.

```cpp
std::vector<Entity> load_entities(World& world, const json::Value& doc, serial::Context& ctx);
```

A prefab is one saved entity object ({"components": {...}}); spawns a fresh copy.

```cpp
Entity spawn_prefab(World& world, const json::Value& entity, serial::Context& ctx);
```

### WorldFile

A saved world file (".world", what the editor writes) as an asset. It hot-reloads like any file; spawn its contents
with load_entities(world, file->document, ctx).

```cpp
struct WorldFile {
    json::Value document;
};
```

### Prefab

A prefab file (".prefab"): one entity ({"components": {...}}), or several as a world document whose entities refer to
each other (a cart and its wheels). Hot-reloads; copies already spawned stay as they are.

```cpp
struct Prefab {
    json::Value document; // always a world document
};
```

Spawns a copy of every entity in the prefab; the first is its root.

```cpp
std::vector<Entity> instantiate(World& world, const Prefab& prefab, serial::Context& ctx);
```

Registers the WorldFile and Prefab loaders. Both read text or binary files.

```cpp
void register_world_file_loader(AssetManager& assets);
```

### Migrations

Upgrades saves written by older versions of a game. Number your save format in serial::Context::game_version (it's
written into every world file) and add one step for each change, before loading:

```cpp
ecs::Migrations migrations;
migrations.add(1, [](json::Value& doc) { ecs::rename_component(doc, "game::Hp", "game::Health"); });  // 1 -> 2
migrations.add(2, [](json::Value& doc) { ecs::rename_field(doc, "game::Health", "hp", "current"); }); // 2 -> 3
migrations.upgrade(doc, ctx);
ecs::load_entities(world, doc, ctx);
```
Files written before a game had a version count as version 0.

```cpp
class Migrations
```

```cpp
using Step = std::function<void(json::Value& document)>;
```

A step that turns a document at `from_version` into from_version + 1.

```cpp
void add(i32 from_version, Step step);
```

The version the last step produces (0 with no steps).

```cpp
i32 latest() const;
```

Runs the steps from the document's version up to latest() and records the new version. False, with a warning,
when a step is missing or the file comes from a newer game than this one.

```cpp
bool upgrade(json::Value& document, serial::Context& ctx) const;
```

The document's "game_version" (0 when missing).

```cpp
i32 game_version_of(const json::Value& document);
```

Helpers for migration steps; each works on every saved entity in a world document.

```cpp
void rename_component(json::Value& document, std::string_view from, std::string_view to);
void remove_component(json::Value& document, std::string_view name);
void rename_field(json::Value& document, std::string_view component, std::string_view from, std::string_view to);
```

Calls `change` with each saved copy of a component, to rework its data.

```cpp
void for_each_saved(json::Value& document, std::string_view component, const std::function<void(json::Value&)>& change);
```

