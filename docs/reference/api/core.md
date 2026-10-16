# core

Made by `bowl-apidoc` from the comments in the headers. Don't edit it here: change the header, then run
`cmake --build <build folder> --target bowl_api_docs`.

- [bowl/core/archive.hpp](#bowlcorearchivehpp)
- [bowl/core/assert.hpp](#bowlcoreasserthpp)
- [bowl/core/assets.hpp](#bowlcoreassetshpp)
- [bowl/core/ease.hpp](#bowlcoreeasehpp)
- [bowl/core/file.hpp](#bowlcorefilehpp)
- [bowl/core/function_ref.hpp](#bowlcorefunction_refhpp)
- [bowl/core/jobs.hpp](#bowlcorejobshpp)
- [bowl/core/json.hpp](#bowlcorejsonhpp)
- [bowl/core/json_text.hpp](#bowlcorejson_texthpp)
- [bowl/core/localization.hpp](#bowlcorelocalizationhpp)
- [bowl/core/log.hpp](#bowlcoreloghpp)
- [bowl/core/math.hpp](#bowlcoremathhpp)
- [bowl/core/profiler.hpp](#bowlcoreprofilerhpp)
- [bowl/core/reflect.hpp](#bowlcorereflecthpp)
- [bowl/core/serialize.hpp](#bowlcoreserializehpp)
- [bowl/core/settings.hpp](#bowlcoresettingshpp)
- [bowl/core/shader_bundle.hpp](#bowlcoreshader_bundlehpp)
- [bowl/core/time.hpp](#bowlcoretimehpp)
- [bowl/core/type_id.hpp](#bowlcoretype_idhpp)
- [bowl/core/types.hpp](#bowlcoretypeshpp)
- [bowl/core/utf8.hpp](#bowlcoreutf8hpp)
- [bowl/core/xml.hpp](#bowlcorexmlhpp)

## bowl/core/archive.hpp

`#include "bowl/core/archive.hpp"` · namespace `bowl`

### Archive

An asset archive (.bowlpak): many files in one, stored as they are, with an index at the front. Shipping builds put
their asset folders in archives (BOWL_PACK_ASSETS) and AssetManager::mount_archive reads from them; bowl-pack makes
them.

Layout, little-endian: "BWPK", u32 version (1), u32 file count, then per file: u16 name length, the name (UTF-8,
'/' separated), u64 offset from the start of the archive, u64 size; then the files' bytes.

```cpp
class Archive : NonCopyable
```

Reads the index. Nothing if the file isn't an archive.

```cpp
static std::unique_ptr<Archive> open(const std::filesystem::path& path);
```

```cpp
bool contains(std::string_view name) const;
```

A file's bytes. Safe from any thread.

```cpp
std::optional<std::vector<u8>> read(std::string_view name) const;
std::vector<std::string> names() const; // sorted
const std::filesystem::path& path() const;
```

Writes an archive of every file under `folder`, named by their paths relative to it. False if a file can't be read
or the archive can't be written.

```cpp
bool write_archive(const std::filesystem::path& archive, const std::filesystem::path& folder);
```

## bowl/core/assert.hpp

`#include "bowl/core/assert.hpp"`

BOWL_ASSERT(cond[, "message"]): checked in debug and dev builds, compiled out when BOWL_ASSERTS is 0.
BOWL_VERIFY(cond[, "message"]): like BOWL_ASSERT, but cond is evaluated even when asserts are off.

```cpp
#define BOWL_ASSERTS 1
#define BOWL_ASSERTS 0
```

```cpp
#define BOWL_ASSERT(cond, ...)
#define BOWL_VERIFY(cond, ...) BOWL_ASSERT(cond __VA_OPT__(, ) __VA_ARGS__)
#define BOWL_ASSERT(cond, ...) ((void)0)
#define BOWL_VERIFY(cond, ...) ((void)(cond))
```

```cpp
#define BOWL_UNREACHABLE() ::bowl::detail::assert_fail("unreachable", __FILE__, __LINE__)
```

## bowl/core/assets.hpp

`#include "bowl/core/assets.hpp"` · namespace `bowl`

### AssetSlot

```cpp
template <class T> struct AssetSlot {
    std::unique_ptr<T> value;
    u32 version = 0;
    std::string path;
};
```

### Asset

Shared handle to a loaded asset. Hot reload swaps the value in place, so every handle sees the new data; compare
version() with a cached copy to rebuild anything derived from it (pipelines, atlases, ...).

```cpp
template <class T> class Asset
```

```cpp
using value_type = T;
```

```cpp
Asset() = default;
explicit Asset(std::shared_ptr<AssetSlot<T>> slot);
```

```cpp
explicit operator bool() const;
T* get() const;
T* operator->() const;
T& operator*() const;
u32 version() const;
std::string_view path() const;
bool operator==(const Asset& o) const{ return slot_ == o.slot_; };
```

### is_asset_handle

```cpp
template <class T> struct is_asset_handle : std::false_type {
};
```

### is_asset_handle

```cpp
template <class T> struct is_asset_handle<Asset<T>> : std::true_type {
};
```

### AssetLoadContext

Passed to loaders. Read files through it so hot reload knows what the asset depends on.

```cpp
class AssetLoadContext
```

```cpp
AssetLoadContext(AssetManager& assets, std::string_view path);
```

The virtual path being loaded, e.g. "textures/hero.png".

```cpp
std::string_view path() const;
AssetManager& assets() const;
```

Reads a file by virtual path from the first mount that has it, and records it as a dependency.

```cpp
std::optional<std::vector<u8>> read(std::string_view virtual_path);
std::optional<std::string> read_text(std::string_view virtual_path);
bool exists(std::string_view virtual_path) const;
```

Records a real file as a dependency (e.g. a shader #include resolved by a compiler).

```cpp
void depends_on(const std::filesystem::path& file);
```

```cpp
const std::vector<std::filesystem::path>& dependencies() const;
```

Background loads (AssetManager::load_async): what the type's decoder made from the file on the loading thread,
or null. The loader then only does what has to happen on the main thread (a GPU upload, say).

```cpp
template <class P> P* prepared() const;
```

```cpp
template <class T> using AssetLoader = std::function<std::unique_ptr<T>(AssetLoadContext&)>;
```

Runs on the loading thread for load_async: turns the file's bytes into whatever saves the loader work (decoded
pixels, decoded samples). Must be thread-safe. Null: nothing to prepare.

```cpp
using AssetDecoder = std::function<std::shared_ptr<void>(std::string_view path, std::span<const u8> bytes)>;
```

### AssetManager

Loads and caches assets by virtual path from an ordered list of mounted folders and archives. Main thread only
(load_async does its file reading and decoding on a thread of its own).
If a reload fails (say, a shader with a syntax error), the previous value stays in place.

```cpp
class AssetManager : NonCopyable
```

```cpp
AssetManager();
~AssetManager();
```

Files under `virtual_prefix` (e.g. "engine/" or "") resolve into `directory`. Earlier mounts win.

```cpp
void mount(std::string virtual_prefix, std::filesystem::path directory);
```

The same, from an archive (.bowlpak, see bowl/core/archive.hpp). False if it isn't one.

```cpp
bool mount_archive(std::string virtual_prefix, const std::filesystem::path& archive);
```

The real file a virtual path reads from, for code that needs one (a font library, a compiler). Nothing when the
file is missing or inside an archive: read() works for both.

```cpp
std::optional<std::filesystem::path> resolve(std::string_view virtual_path) const;
bool exists(std::string_view virtual_path) const;
```

A file's bytes by virtual path, from whichever mount has it. Not cached and not watched: load assets with load().

```cpp
std::optional<std::vector<u8>> read(std::string_view virtual_path) const;
```

```cpp
template <class T> void register_loader(AssetLoader<T> loader);
```

Returns the cached asset or loads it. On failure the handle is empty (and the error is logged); if the file
appears or is fixed later, hot reload fills it in. An asset still loading in the background is finished now.

```cpp
template <class T> Asset<T> load(std::string_view path);
```

Starts loading in the background and returns the handle at once: the file is read, and decoded if the type has
a decoder, on a loading thread; finish_loads() (the App calls it every frame) completes it on this thread. The
handle stays empty until then, so check it before use. Loading screens: start everything, wait for loading()
to reach zero.

```cpp
template <class T> Asset<T> load_async(std::string_view path);
template <class T> void register_decoder(AssetDecoder decoder);
```

Background loads not finished yet.

```cpp
usize loading() const;
```

Completes the background loads that are ready.

```cpp
void finish_loads();
```

Registers an asset made in code (procedural textures, generated sounds). Never reloaded.

```cpp
template <class T> Asset<T> add(std::string_view path, std::unique_ptr<T> value);
```

```cpp
template <class T> Asset<T> find(std::string_view path) const;
```

Re-runs the loader of every asset whose files changed. Call from the main loop; cheap when nothing changed.

```cpp
void set_hot_reload(bool enabled);
bool hot_reload() const;
void poll();
```

Reloads one asset now (dev tools button). Returns false if it failed or isn't loaded.

```cpp
bool reload(std::string_view path);
void on_reload(std::function<void(std::string_view path, bool ok)> callback);
```

rollback(mark) drops every loader, asset and callback registered after mark() was taken, and restores loaders
they replaced. Game-code hot reload uses it to free the game library's objects before unloading the library.

```cpp
u64 mark() const;
void rollback(u64 mark);
```

#### Info

```cpp
struct Info {
    std::string path;
    std::string_view type;
    u32 version;
    bool loaded;
};
```

```cpp
std::vector<Info> list() const;
```

```cpp
void log_asset_error(std::string_view path, std::string_view type, std::string_view message);
```

## bowl/core/ease.hpp

`#include "bowl/core/ease.hpp"` · namespace `bowl`

Easing curves: t in 0..1 -> 0..1 (some overshoot, like back and elastic).

```cpp
enum class Ease : u8 {
    Linear,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InSine, OutSine, InOutSine,
    InExpo, OutExpo, InOutExpo,
    InBack, OutBack, InOutBack,
    InElastic, OutElastic, InOutElastic,
    InBounce, OutBounce, InOutBounce,
};
```

```cpp
inline f32 ease(Ease e, f32 t);
```

### Tween

Animates a value from `from` to `to` over `duration` seconds. Works for anything lerp() supports (f32, Vec2,
Color, ...). Plain data: keep it in a component and step it in a system.

```cpp
Tween<Vec2> slide{{0, 0}, {200, 0}, 0.4f, Ease::OutBack};
panel.position = slide.update(dt);
```

```cpp
template <class T> struct Tween
```

```cpp
T from{};
, to{};
f32 duration = 1;
Ease curve = Ease::Linear;
f32 elapsed = 0;
f32 delay = 0;
```

```cpp
T value() const;
T update(f32 dt);
bool done() const;
void restart();
```

Plays backwards from wherever it is now (e.g. a panel closing halfway through opening).

```cpp
void reverse();
```

## bowl/core/file.hpp

`#include "bowl/core/file.hpp"` · namespace `bowl`

```cpp
std::optional<std::vector<u8>> read_file_bytes(const std::filesystem::path& path);
std::optional<std::string> read_file_text(const std::filesystem::path& path);
bool write_file_bytes(const std::filesystem::path& path, const void* data, usize size);
```

### FileWatcher

Polls modification times. Portable (no OS watch APIs) and cheap at a few hundred files; call poll() a few times a second.

```cpp
class FileWatcher
```

```cpp
using Callback = std::function<void(const std::filesystem::path&)>;
using WatchId = u32;
```

```cpp
WatchId watch(const std::filesystem::path& path, Callback on_change);
void unwatch(WatchId id);
```

Calls the callback of each file whose modification time changed since the last poll.

```cpp
void poll();
usize size() const;
```

## bowl/core/function_ref.hpp

`#include "bowl/core/function_ref.hpp"` · namespace `bowl`

```cpp
template <class R, class... Args> class FunctionRef<R(Args...)>;
```

## bowl/core/jobs.hpp

`#include "bowl/core/jobs.hpp"` · namespace `bowl`

### TaskGroup

Counts outstanding jobs. Wait on it with JobSystem::wait, which helps run jobs instead of sleeping.

```cpp
class TaskGroup
```

```cpp
bool done() const;
```

### JobSystem

A fixed pool of worker threads. A thread waiting on a TaskGroup runs queued jobs itself, so a pool with zero
workers still works: everything runs inline on the caller (useful for single-core targets and tests).

```cpp
class JobSystem : NonCopyable
```

workers &lt; 0: hardware threads - 1.

```cpp
explicit JobSystem(i32 workers = -1);
~JobSystem();
```

```cpp
u32 worker_count() const;
```

Workers plus the calling thread.

```cpp
u32 concurrency() const;
```

```cpp
void run(TaskGroup& group, std::function<void()> job);
void wait(TaskGroup& group);
```

Calls fn(begin, end) over [0, count) in contiguous chunks of at least min_chunk items and returns when all are
done. Chunk boundaries depend only on count and min_chunk, not on the number of threads, so results collected
per chunk can be merged in a deterministic order.

```cpp
void parallel_for(usize count, usize min_chunk, FunctionRef<void(usize begin, usize end)> fn);
```

Items per chunk parallel_for uses for this count; chunk i covers [i*size, min(count, (i+1)*size)).

```cpp
static usize chunk_size(usize count, usize min_chunk);
```

## bowl/core/json.hpp

`#include "bowl/core/json.hpp"` · namespace `bowl::json`

```cpp
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // keeps file order, so saved files diff cleanly
```

### Value

A JSON document node. Lookups never throw: a missing key or a wrong type gives a null Value or the fallback you
pass, so optional fields read in one expression: sheet["meta"]["size"]["w"].as_int(0)

```cpp
class Value
```

```cpp
Value() = default;
Value(std::nullptr_t);
Value(bool b);
Value(f64 n);
Value(int n);
Value(i64 n);
Value(u32 n);
Value(f32 n);
Value(const char* s);
Value(std::string s);
Value(std::string_view s);
Value(Array a);
Value(Object o);
```

```cpp
bool is_null() const;
bool is_bool() const;
bool is_number() const;
bool is_string() const;
bool is_array() const;
bool is_object() const;
```

```cpp
bool as_bool(bool fallback = false) const;
f64 as_number(f64 fallback = 0) const;
f32 as_float(f32 fallback = 0) const;
i64 as_int(i64 fallback = 0) const;
std::string_view as_string(std::string_view fallback = {}) const;
```

Arrays and objects. Out-of-range or missing gives a shared null.

```cpp
usize size() const;
const Value& operator[](usize index) const;
const Value& operator[](std::string_view key) const;
const Value* find(std::string_view key) const;
bool contains(std::string_view key) const;
const Array& array() const;
const Object& object() const;
```

Building and changing documents. make_array/make_object keep the contents when the value already is one.

```cpp
Array& make_array();
Object& make_object();
Value& set(std::string_view key, Value value); // adds or replaces
void push(Value value);
Value* find(std::string_view key);
bool erase(std::string_view key); // false if there was no such key
```

```cpp
bool operator==(const Value&) const = default;
```

### ParseError

```cpp
struct ParseError {
    std::string message;
    u32 line = 0, column = 0;
};
```

### ParseResult

```cpp
struct ParseResult
```

```cpp
Value value;
std::optional<ParseError> error;
explicit operator bool() const;
```

Strict JSON, plus two conveniences for hand-written files: // and /* */ comments, and trailing commas.

```cpp
ParseResult parse(std::string_view text);
```

indent &lt; 0: compact one line; otherwise pretty-printed with that many spaces.

```cpp
std::string stringify(const Value& value, int indent = 2);
```

A compact binary form of a document, for big saves: the same values, with every key and string stored once and
numbers in as few bytes as they need. Several times smaller than text, and quicker to read. Starts "BWJB".

```cpp
std::vector<u8> encode_binary(const Value& value);
ParseResult decode_binary(std::span<const u8> data); // errors give the byte offset as the column
bool is_binary(std::span<const u8> data);
```

Reads either form.

```cpp
ParseResult parse_any(std::span<const u8> data);
```

## bowl/core/json_text.hpp

`#include "bowl/core/json_text.hpp"` · namespace `bowl::json`

### TextDocument

A JSON document edited as text: each change rewrites only the value it touches, so comments, spacing, key order and
trailing commas stay as the person who wrote the file left them. For tools that edit files people also edit by hand.

```cpp
TextDocument doc;
doc.load(*read_file_text("data/enemies.json"));
doc.set({"slime", "hp"}, 40);
doc.insert({"slime", "drops"}, "gel");
write_file_bytes(path, doc.text().data(), doc.text().size());
```

```cpp
class TextDocument
```

A key in an object or an index in an array, from the root down.

```cpp
using Step = std::variant<std::string, usize>;
using Path = std::vector<Step>;
```

False (with the parse error) if the text isn't valid JSON; the text is kept either way so it can be fixed.

```cpp
bool load(std::string text);
const std::string& text() const;
const Value& value() const;
bool valid() const;
const std::optional<ParseError>& error() const;
```

Replaces the value at an existing path.

```cpp
bool set(const Path& path, const Value& value);
```

Appends to the array at `container`, or adds `key` to the object there (replacing it if it exists).

```cpp
bool insert(const Path& container, const Value& value, std::string_view key = {});
```

Removes an item or a key and its value, with the comma that went with it.

```cpp
bool erase(const Path& path);
```

## bowl/core/localization.hpp

`#include "bowl/core/localization.hpp"` · namespace `bowl`

### StringTable

One language's strings, from a JSON file (assets "lang/&lt;code>.json"). Nested objects become dotted keys:
```cpp
{ "menu": { "start": "Start", "quit": "Quit" }, "coins": { "one": "{n} coin", "other": "{n} coins" } }
```

```cpp
class StringTable
```

```cpp
std::unordered_map<std::string, std::string> strings;
const std::string* find(std::string_view key) const;
```

```cpp
static bool parse(std::string_view json_text, StringTable& out, std::string& error);
```

### Localization

Looks strings up in the current language, then the fallback, then shows the key itself so gaps are visible.
Tables hot-reload like any asset.

```cpp
i18n.set_language("es");
draw_text(..., i18n.tr("menu.start"));
i18n.format("greeting", {{"name", player}});      // "Hello, {name}!"
i18n.plural("coins", 3);                           // "coins.one" / "coins.other", with {n} filled in
```

```cpp
class Localization
```

```cpp
explicit Localization(AssetManager& assets);
```

```cpp
void set_language(std::string_view code);
void set_fallback(std::string_view code);
const std::string& language() const;
```

```cpp
std::string_view tr(std::string_view key) const;
std::string format(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> args) const;
std::string plural(std::string_view key, i64 count) const;
bool has(std::string_view key) const;
```

Replaces {name} placeholders. Unknown placeholders are left as they are.

```cpp
std::string fill_placeholders(std::string_view text, std::initializer_list<std::pair<std::string_view, std::string_view>> args);
```

```cpp
void register_string_table_loader(AssetManager& assets);
```

## bowl/core/log.hpp

`#include "bowl/core/log.hpp"` · namespace `bowl`, `logging`

```cpp
enum class LogLevel : u8 { Trace, Debug, Info, Warn, Error };
```

### LogLine

```cpp
struct LogLine {
    LogLevel level;
    f64 time; // seconds since start
    std::string text;
};
```

```cpp
using LogSink = void (*)(LogLevel level, std::string_view text, void* user);
```

```cpp
void set_level(LogLevel level);
LogLevel level();
```

Sinks receive every line at or above the level, from any thread (calls are serialized).

```cpp
void add_sink(LogSink sink, void* user);
void remove_sink(LogSink sink, void* user);
void write(LogLevel level, std::string_view text);
```

The most recent lines, oldest first (for the dev console).

```cpp
std::vector<LogLine> recent(usize max_lines = 512);
```

```cpp
template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args);
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args);
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args);
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args);
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args);
```

## bowl/core/math.hpp

`#include "bowl/core/math.hpp"` · namespace `bowl`

```cpp
inline constexpr f32 pi = std::numbers::pi_v<f32>;
inline constexpr f32 tau = 2.0f * pi;
```

```cpp
constexpr f32 radians(f32 degrees);
constexpr f32 degrees(f32 radians);
template <class T> constexpr T lerp(T a, T b, f32 t);
template <class T> constexpr T clamp(T v, T lo, T hi);
constexpr f32 saturate(f32 v);
```

### Vec2

```cpp
struct Vec2
```

```cpp
f32 x = 0, y = 0;
```

```cpp
constexpr Vec2() = default;
constexpr Vec2(f32 x_, f32 y_);
constexpr explicit Vec2(f32 s);
```

```cpp
constexpr Vec2 operator+(Vec2 o) const;
constexpr Vec2 operator-(Vec2 o) const;
constexpr Vec2 operator*(Vec2 o) const;
constexpr Vec2 operator/(Vec2 o) const;
constexpr Vec2 operator*(f32 s) const;
constexpr Vec2 operator/(f32 s) const;
constexpr Vec2 operator-() const;
constexpr Vec2& operator+=(Vec2 o){ x += o.x; y += o.y; return *this; };
constexpr Vec2& operator-=(Vec2 o){ x -= o.x; y -= o.y; return *this; };
constexpr Vec2& operator*=(f32 s){ x *= s; y *= s; return *this; };
constexpr Vec2& operator/=(f32 s){ x /= s; y /= s; return *this; };
constexpr bool operator==(const Vec2&) const = default;
```

```cpp
constexpr f32 dot(Vec2 o) const;
constexpr f32 cross(Vec2 o) const;
constexpr f32 length_squared() const;
f32 length() const;
Vec2 normalized() const;
f32 distance(Vec2 o) const;
f32 angle() const;
constexpr Vec2 perpendicular() const;
Vec2 rotated(f32 angle) const;
static Vec2 from_angle(f32 angle);
```

```cpp
constexpr Vec2 operator*(f32 s, Vec2 v);
```

### Vec3

```cpp
struct Vec3
```

```cpp
f32 x = 0, y = 0, z = 0;
```

```cpp
constexpr Vec3() = default;
constexpr Vec3(f32 x_, f32 y_, f32 z_);
constexpr Vec3(Vec2 v, f32 z_);
constexpr explicit Vec3(f32 s);
```

```cpp
constexpr Vec3 operator+(Vec3 o) const;
constexpr Vec3 operator-(Vec3 o) const;
constexpr Vec3 operator*(Vec3 o) const;
constexpr Vec3 operator*(f32 s) const;
constexpr Vec3 operator/(f32 s) const;
constexpr Vec3 operator-() const;
constexpr Vec3& operator+=(Vec3 o){ x += o.x; y += o.y; z += o.z; return *this; };
constexpr Vec3& operator-=(Vec3 o){ x -= o.x; y -= o.y; z -= o.z; return *this; };
constexpr Vec3& operator*=(f32 s){ x *= s; y *= s; z *= s; return *this; };
constexpr bool operator==(const Vec3&) const = default;
```

```cpp
constexpr f32 dot(Vec3 o) const;
constexpr Vec3 cross(Vec3 o) const;
constexpr f32 length_squared() const;
f32 length() const;
Vec3 normalized() const;
constexpr Vec2 xy() const;
```

### Vec4

```cpp
struct Vec4
```

```cpp
f32 x = 0, y = 0, z = 0, w = 0;
```

```cpp
constexpr Vec4() = default;
constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_);
constexpr Vec4(Vec3 v, f32 w_);
```

```cpp
constexpr Vec4 operator+(Vec4 o) const;
constexpr Vec4 operator-(Vec4 o) const;
constexpr Vec4 operator*(f32 s) const;
constexpr bool operator==(const Vec4&) const = default;
constexpr f32 dot(Vec4 o) const;
constexpr Vec3 xyz() const;
```

### Mat4

Column-major 4x4 matrix (matches GLSL / std140), for column vectors: v' = M * v.

```cpp
struct Mat4
```

```cpp
Vec4 cols[4] ={{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
```

```cpp
static constexpr Mat4 identity();
```

```cpp
constexpr Vec4 row(int r) const;
constexpr Mat4 operator*(const Mat4& o) const;
constexpr Vec4 operator*(Vec4 v) const;
constexpr Vec2 transform_point(Vec2 p) const;
```

```cpp
static constexpr Mat4 translation(Vec3 t);
static constexpr Mat4 scale(Vec3 s);
static Mat4 rotation_z(f32 angle);
```

Orthographic projection to SDL_GPU clip space (x,y in [-1,1] with +y up, z in [0,1]).

```cpp
static constexpr Mat4 ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near_z = -1, f32 far_z = 1);
constexpr const f32* data() const;
```

### Rect

```cpp
struct Rect
```

```cpp
f32 x = 0, y = 0, w = 0, h = 0;
```

```cpp
static constexpr Rect from_center(Vec2 center, Vec2 size);
constexpr Rect expanded(f32 by) const;
constexpr Vec2 position() const;
constexpr Vec2 size() const;
constexpr Vec2 center() const;
constexpr f32 right() const;
constexpr f32 bottom() const;
constexpr bool contains(Vec2 p) const;
constexpr bool overlaps(const Rect& o) const;
constexpr bool operator==(const Rect&) const = default;
```

### Color

RGBA with float channels, normally 0..1. Values above 1 are allowed and make sprites glow when bloom is on.
Color::hex(0xRRGGBB) and Color::rgba8 convert from the usual 8-bit notations.

```cpp
struct Color
```

```cpp
f32 r = 1, g = 1, b = 1, a = 1;
```

```cpp
static constexpr Color rgba8(u8 red, u8 green, u8 blue, u8 alpha = 255);
static constexpr Color hex(u32 rgb, f32 alpha = 1.0f);
constexpr Color with_alpha(f32 alpha) const;
constexpr Color premultiplied() const;
constexpr bool operator==(const Color&) const = default;
```

```cpp
static constexpr Color white();
static constexpr Color black();
static constexpr Color transparent();
```

```cpp
constexpr Color lerp(Color a, Color b, f32 t);
```

## bowl/core/profiler.hpp

`#include "bowl/core/profiler.hpp"` · namespace `bowl`, `profiler`

### ZoneStats

```cpp
struct ZoneStats {
    std::string_view name;
    f64 milliseconds = 0; // total across all calls and threads this frame
    u32 calls = 0;
};
```

```cpp
void set_enabled(bool enabled);
bool enabled();
void begin_frame();
void end_frame();
void record(std::string_view name, f64 milliseconds);
```

Stats of the last completed frame, sorted by time descending.

```cpp
std::vector<ZoneStats> last_frame();
```

Total frame time history (ms), oldest first.

```cpp
std::vector<f32> frame_history();
```

Tracy only (no-ops otherwise). Plot names must be string literals or otherwise live forever.

```cpp
void set_thread_name(const char* name);
void plot(const char* name, f64 value);
bool tracy_enabled();
```

### ProfileScope

```cpp
class ProfileScope
```

```cpp
explicit ProfileScope(std::string_view name, const char* file = nullptr, u32 line = 0);
~ProfileScope();
ProfileScope(const ProfileScope&) = delete;
ProfileScope& operator=(const ProfileScope&) = delete;
```

```cpp
#define BOWL_PROFILE_SCOPE(name)
```

## bowl/core/reflect.hpp

`#include "bowl/core/reflect.hpp"` · namespace `bowl::reflect`

Reflection without code generation. After a struct, at namespace scope (in the struct's own namespace):

```cpp
struct Health { i32 current = 100; i32 max = 100; bool invulnerable = false; };
BOWL_REFLECT(Health, current, max, invulnerable)
```
Reflected types serialize to JSON, save and load with the world, and are editable live in the dev overlay.
Up to 32 fields per type.

### Field

```cpp
template <class C, class M> struct Field {
    std::string_view name;
    M C::*member;
};
```

### TypeInfo

```cpp
template <class... Fields> struct TypeInfo {
    std::string_view name;
    std::tuple<Fields...> fields;
};
```

```cpp
template <class... Fields> TypeInfo(std::string_view, std::tuple<Fields...>) -> TypeInfo<Fields...>;
```

### FieldVisitor

Live editing (the dev overlay's inspector) walks values through this interface; values are passed by reference
and changes are written back. Unsupported field types are reported by name so they still show up.

```cpp
class FieldVisitor
```

```cpp
virtual ~FieldVisitor() = default;
virtual void boolean(std::string_view name, bool& value) = 0;
virtual void integer(std::string_view name, i64& value, i64 min, i64 max) = 0;
virtual void real(std::string_view name, f64& value) = 0;
virtual void text(std::string_view name, std::string& value) = 0;
virtual void vector(std::string_view name, f32* values, int count) = 0;
virtual void color(std::string_view name, Color& value) = 0;
virtual void entity(std::string_view name, u64 bits) = 0; // read-only
```

The same, for visitors that change entity references (the ECS swaps stand-in ids for real ones this way).

```cpp
virtual void entity_ref(std::string_view name, u64& bits);
virtual bool begin_group(std::string_view name) = 0;      // false: skip its fields
virtual void end_group() = 0;
virtual void unsupported(std::string_view name, std::string_view type) = 0;
```

Asset&lt;T> fields. `path` is the asset's path ("" for none); change it and return true to load that path instead,
which needs assets() to return the manager to load from.

```cpp
virtual bool asset(std::string_view name, std::string& path, std::string_view type);
virtual AssetManager* assets();
```

std::vector and std::array fields. `size` is how many items there are; a visitor may change it for resizable
lists (vectors) to add or remove items at the end. Return false to skip the items.

```cpp
virtual bool begin_list(std::string_view name, usize& size, bool resizable);
virtual void end_list();
```

```cpp
template <class T> void visit(FieldVisitor& v, std::string_view name, T& value);
```

Found by argument-dependent lookup: BOWL_REFLECT defines bowl_reflect_info(const T*) next to T.

```cpp
template <class T>
concept Reflected = requires(const T* p){ bowl_reflect_info(p); };
```

```cpp
template <Reflected T> constexpr auto info();
```

Calls f(name, member) for each reflected field of obj (const or not).

```cpp
template <class T, class F>
    requires Reflected<std::remove_const_t<T>>
void for_each_field(T& obj, F&& f);
```

```cpp
template <Reflected T> constexpr usize field_count();
```

Types that know how to show themselves (e.g. ecs::Entity) provide bowl_visit(FieldVisitor&, name, T&) via ADL.

```cpp
template <class T>
concept HasVisitHook = requires(FieldVisitor& v, std::string_view n, T& t){ bowl_visit(v, n, t); };
```

```cpp
template <class T> void visit(FieldVisitor& v, std::string_view name, T& value);
```

```cpp
#define BOWL_EXPAND(x) x
#define BOWL_FE_1(m, x) m(x)
#define BOWL_FE_2(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_1(m, __VA_ARGS__))
#define BOWL_FE_3(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_2(m, __VA_ARGS__))
#define BOWL_FE_4(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_3(m, __VA_ARGS__))
#define BOWL_FE_5(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_4(m, __VA_ARGS__))
#define BOWL_FE_6(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_5(m, __VA_ARGS__))
#define BOWL_FE_7(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_6(m, __VA_ARGS__))
#define BOWL_FE_8(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_7(m, __VA_ARGS__))
#define BOWL_FE_9(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_8(m, __VA_ARGS__))
#define BOWL_FE_10(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_9(m, __VA_ARGS__))
#define BOWL_FE_11(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_10(m, __VA_ARGS__))
#define BOWL_FE_12(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_11(m, __VA_ARGS__))
#define BOWL_FE_13(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_12(m, __VA_ARGS__))
#define BOWL_FE_14(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_13(m, __VA_ARGS__))
#define BOWL_FE_15(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_14(m, __VA_ARGS__))
#define BOWL_FE_16(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_15(m, __VA_ARGS__))
#define BOWL_FE_17(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_16(m, __VA_ARGS__))
#define BOWL_FE_18(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_17(m, __VA_ARGS__))
#define BOWL_FE_19(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_18(m, __VA_ARGS__))
#define BOWL_FE_20(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_19(m, __VA_ARGS__))
#define BOWL_FE_21(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_20(m, __VA_ARGS__))
#define BOWL_FE_22(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_21(m, __VA_ARGS__))
#define BOWL_FE_23(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_22(m, __VA_ARGS__))
#define BOWL_FE_24(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_23(m, __VA_ARGS__))
#define BOWL_FE_25(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_24(m, __VA_ARGS__))
#define BOWL_FE_26(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_25(m, __VA_ARGS__))
#define BOWL_FE_27(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_26(m, __VA_ARGS__))
#define BOWL_FE_28(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_27(m, __VA_ARGS__))
#define BOWL_FE_29(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_28(m, __VA_ARGS__))
#define BOWL_FE_30(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_29(m, __VA_ARGS__))
#define BOWL_FE_31(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_30(m, __VA_ARGS__))
#define BOWL_FE_32(m, x, ...) m(x), BOWL_EXPAND(BOWL_FE_31(m, __VA_ARGS__))
#define BOWL_FE_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, NAME, ...) NAME
#define BOWL_FOR_EACH(m, ...) BOWL_EXPAND(BOWL_FE_PICK(__VA_ARGS__, BOWL_FE_32, BOWL_FE_31, BOWL_FE_30, BOWL_FE_29, BOWL_FE_28, BOWL_FE_27, BOWL_FE_26, BOWL_FE_25, BOWL_FE_24, BOWL_FE_23, BOWL_FE_22, BOWL_FE_21, BOWL_FE_20, BOWL_FE_19, BOWL_FE_18, BOWL_FE_17, BOWL_FE_16, BOWL_FE_15, BOWL_FE_14, BOWL_FE_13, BOWL_FE_12, BOWL_FE_11, BOWL_FE_10, BOWL_FE_9, BOWL_FE_8, BOWL_FE_7, BOWL_FE_6, BOWL_FE_5, BOWL_FE_4, BOWL_FE_3, BOWL_FE_2, BOWL_FE_1)(m, __VA_ARGS__))
```

```cpp
#define BOWL_REFLECT_FIELD(f)
```

```cpp
#define BOWL_REFLECT(Type, ...)
```

## bowl/core/serialize.hpp

`#include "bowl/core/serialize.hpp"` · namespace `bowl::serial`

### Context

State shared across one save or load.

```cpp
struct Context
```

```cpp
AssetManager* assets = nullptr;                     // to reload Asset<T> fields by path
std::unordered_map<u64, u64>* entity_map = nullptr; // runtime entity <-> file id (see ecs/world_io.hpp)
std::vector<std::string> warnings;                  // problems that didn't stop the load
```

The game's own save version, written into world files as "game_version" when above 0 (see ecs::Migrations).

```cpp
i32 game_version = 0;
void warn(std::string message);
```

What can be saved: bool, numbers, enums (as numbers), std::string, Vec2/3/4, Color, Rect, std::vector, std::array,
std::optional, Asset&lt;T> (as its path), reflected structs (as objects), and any type with ADL hooks
```cpp
json::Value bowl_to_json(const T&, serial::Context&);
bool bowl_from_json(const json::Value&, T&, serial::Context&);
```
Loading is tolerant: missing keys keep the current value, so adding fields never breaks old save files.

```cpp
template <class T> json::Value to_json(const T& value, Context& ctx);
template <class T> bool from_json(const json::Value& j, T& value, Context& ctx);
```

### is_vector

```cpp
template <class T> struct is_vector : std::false_type {
};
```

### is_vector

```cpp
template <class T, class A> struct is_vector<std::vector<T, A>> : std::true_type {
};
```

### is_array

```cpp
template <class T> struct is_array : std::false_type {
};
```

### is_array

```cpp
template <class T, usize N> struct is_array<std::array<T, N>> : std::true_type {
};
```

### is_optional

```cpp
template <class T> struct is_optional : std::false_type {
};
```

### is_optional

```cpp
template <class T> struct is_optional<std::optional<T>> : std::true_type {
};
```

### is_asset

```cpp
template <class T> struct is_asset : std::false_type {
};
```

### is_asset

```cpp
template <class T> struct is_asset<Asset<T>> : std::true_type {
};
```

```cpp
template <class T>
concept HasHooks = requires(const T& c, T& m, const json::Value& j, Context& ctx){
    { bowl_to_json(c, ctx) } -> std::convertible_to<json::Value>;
    { bowl_from_json(j, m, ctx) } -> std::convertible_to<bool>;
};
```

```cpp
template <class T>
concept Serializable = std::is_same_v<T, json::Value> || std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                       std::is_same_v<T, std::string> ||
                       std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4> ||
                       std::is_same_v<T, Color> || std::is_same_v<T, Rect> || is_vector<T>::value ||
                       is_array<T>::value || is_optional<T>::value || is_asset<T>::value || reflect::Reflected<T> ||
                       HasHooks<T>;
```

```cpp
template <class T> json::Value to_json(const T& v, Context& ctx);
```

```cpp
template <class T> bool from_json(const json::Value& j, T& v, Context& ctx);
```

## bowl/core/settings.hpp

`#include "bowl/core/settings.hpp"` · namespace `bowl`

### Settings

Player preferences that outlive the game: volume, language, window mode, controls. Stored as JSON in the OS's
per-user app data folder (engine.settings() is loaded at startup and saved at exit).

```cpp
f32 music = engine.settings().get("audio.music", 0.8f);
engine.settings().set("audio.music", music);
```
Dotted keys address nested objects. Values use the serializer, so reflected structs work too.

```cpp
class Settings
```

```cpp
bool load(const std::filesystem::path& file);
bool save(); // writes atomically (temp file + rename), and only if something changed
const std::filesystem::path& file() const;
```

```cpp
template <class T> T get(std::string_view key, T fallback) const;
template <class T> void set(std::string_view key, const T& value);
bool has(std::string_view key) const;
const json::Value* find(std::string_view key) const;
json::Value& slot(std::string_view key); // for writing: creates objects along the way, marks settings changed
const json::Value& data() const;
bool dirty() const;
```

## bowl/core/shader_bundle.hpp

`#include "bowl/core/shader_bundle.hpp"` · namespace `bowl`

```cpp
enum class ShaderStage : u32 { Vertex = 0, Fragment = 1, Compute = 2 };
```

Hlsl is source (Shader Model 5.1, SDL's register layout); on Windows it's compiled to Dxbc when the D3D12 backend
runs, since the HLSL compiler ships with Windows but not with other build hosts.

```cpp
enum class ShaderBlobFormat : u32 { SpirV = 1, Msl = 2, Dxil = 3, Dxbc = 4, Hlsl = 5 };
```

### ShaderBundle

One shader stage compiled for every GPU backend, plus the resource counts SDL_GPU needs to create it.
Written by bowl-shaderc (build time, or at runtime in dev builds); read by the renderer. Format: little-endian u32s.

```cpp
struct ShaderBundle
```

```cpp
static constexpr u32 magic = 0x48535742; // "BWSH"
static constexpr u32 format_version = 2; // 2 added the compute fields; version 1 bundles still load
```

#### Blob

```cpp
struct Blob {
    ShaderBlobFormat format;
    std::string entry_point;
    std::vector<u8> code;
};
```

```cpp
ShaderStage stage = ShaderStage::Vertex;
u32 samplers = 0;
u32 storage_textures = 0;
u32 storage_buffers = 0;
u32 uniform_buffers = 0;
```

Compute shaders: storage_textures/storage_buffers above count the read-only ones; these the read-write ones.

```cpp
u32 readwrite_storage_textures = 0;
u32 readwrite_storage_buffers = 0;
u32 threads[3] ={1, 1, 1}; // local_size_x/y/z
std::vector<Blob> blobs;
```

```cpp
const Blob* find(ShaderBlobFormat f) const;
```

```cpp
std::vector<u8> serialize() const;
```

```cpp
static std::optional<ShaderBundle> parse(std::span<const u8> data);
```

## bowl/core/time.hpp

`#include "bowl/core/time.hpp"` · namespace `bowl`

Seconds since the process started, from a monotonic clock.

```cpp
f64 now_seconds();
```

### Stopwatch

```cpp
class Stopwatch
```

```cpp
Stopwatch();
void reset();
f64 seconds() const;
f64 milliseconds() const;
```

## bowl/core/type_id.hpp

`#include "bowl/core/type_id.hpp"` · namespace `bowl`

Compile-time type name, e.g. type_name&lt;game::Health>() == "game::Health". Used for debugging and the inspector.

```cpp
template <class T> constexpr std::string_view type_name();
```

```cpp
constexpr u64 fnv1a(std::string_view s);
```

Stable 64-bit id derived from the type's name: identical across runs and builds of the same code.

```cpp
template <class T> constexpr u64 type_hash();
```

Identifies a type in maps (resources, asset loaders). Derived from the type's name, so it's the same in every
module of the process (a hot-reloaded game library and its host agree on it) and across runs.

```cpp
using TypeId = u64;
template <class T> constexpr TypeId type_id();
```

## bowl/core/types.hpp

`#include "bowl/core/types.hpp"` · namespace `bowl`

```cpp
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;
```

### NonCopyable

Non-copyable, non-movable base for objects with identity (devices, worlds, windows).

```cpp
class NonCopyable
```

```cpp
NonCopyable() = default;
NonCopyable(const NonCopyable&) = delete;
NonCopyable& operator=(const NonCopyable&) = delete;
NonCopyable(NonCopyable&&) = delete;
NonCopyable& operator=(NonCopyable&&) = delete;
```

```cpp
#define BOWL_CONCAT_IMPL(a, b) a##b
#define BOWL_CONCAT(a, b) BOWL_CONCAT_IMPL(a, b)
```

## bowl/core/utf8.hpp

`#include "bowl/core/utf8.hpp"` · namespace `bowl::utf8`

```cpp
inline constexpr u32 replacement = 0xFFFD;
```

Decodes the code point at s[i] and advances i past it. Malformed bytes decode as U+FFFD (one byte at a time), so
broken text renders visibly instead of hanging or crashing.

```cpp
constexpr u32 next(std::string_view s, usize& i);
```

```cpp
inline void append(std::string& out, u32 cp);
```

## bowl/core/xml.hpp

`#include "bowl/core/xml.hpp"` · namespace `bowl::xml`

### Element

One element of an XML document. Enough XML for tool formats like Tiled's .tmx: elements, attributes, text,
comments, CDATA and the standard entities. No namespaces, DTDs or validation.

```cpp
struct Element
```

```cpp
std::string name;
std::vector<std::pair<std::string, std::string>> attributes; // in file order, entities decoded
std::vector<Element> children;
std::string text; // character data directly inside this element, joined
```

```cpp
std::string_view attribute(std::string_view key, std::string_view fallback = {}) const;
bool has(std::string_view key) const;
```

The first child with this name, or null.

```cpp
const Element* child(std::string_view child_name) const;
```

### ParseError

```cpp
struct ParseError {
    std::string message;
    u32 line = 0, column = 0;
};
```

### ParseResult

```cpp
struct ParseResult
```

```cpp
Element root;
std::optional<ParseError> error;
explicit operator bool() const;
```

```cpp
ParseResult parse(std::string_view text);
```

