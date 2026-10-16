#include "bowl/core/archive.hpp"
#include "bowl/core/assets.hpp"
#include "bowl/core/file.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace bowl;
namespace fs = std::filesystem;

namespace {

struct Text {
    std::string value;
};

fs::path temp_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& p, std::string_view s) { REQUIRE(write_file_bytes(p, s.data(), s.size())); }

// File systems with coarse timestamps need the new write to land on a different tick.
void bump_mtime(const fs::path& p) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
}

void register_text(AssetManager& assets) {
    assets.register_loader<Text>([](AssetLoadContext& ctx) -> std::unique_ptr<Text> {
        auto text = ctx.read_text(ctx.path());
        if (!text || text->find("ERROR") != std::string::npos) return nullptr;
        return std::make_unique<Text>(Text{*text});
    });
}

} // namespace

TEST_CASE("assets: mounts resolve in order") {
    fs::path a = temp_dir("mount_a"), b = temp_dir("mount_b");
    write(a / "x.txt", "from a");
    write(b / "x.txt", "from b");
    write(b / "y.txt", "only b");
    AssetManager assets;
    assets.mount("", a);
    assets.mount("", b);
    register_text(assets);
    CHECK(assets.load<Text>("x.txt")->value == "from a");
    CHECK(assets.load<Text>("y.txt")->value == "only b");
    CHECK_FALSE(assets.load<Text>("missing.txt"));
}

TEST_CASE("assets: cached by path; hot reload swaps in place; failed reload keeps the old value") {
    fs::path dir = temp_dir("reload");
    write(dir / "t.txt", "one");
    AssetManager assets;
    assets.mount("data/", dir);
    register_text(assets);
    assets.set_hot_reload(true);

    Asset<Text> a = assets.load<Text>("data/t.txt");
    Asset<Text> again = assets.load<Text>("data/t.txt");
    CHECK(a == again);
    CHECK(a->value == "one");
    u32 v1 = a.version();

    write(dir / "t.txt", "two");
    bump_mtime(dir / "t.txt");
    assets.poll();
    CHECK(a->value == "two");
    CHECK(a.version() == v1 + 1);

    write(dir / "t.txt", "ERROR");
    fs::last_write_time(dir / "t.txt", fs::last_write_time(dir / "t.txt") + std::chrono::seconds(4));
    assets.poll();
    CHECK(a->value == "two"); // kept
}

TEST_CASE("assets: a missing file that appears later is picked up") {
    fs::path dir = temp_dir("appear");
    AssetManager assets;
    assets.mount("", dir);
    register_text(assets);
    assets.set_hot_reload(true);
    Asset<Text> a = assets.load<Text>("later.txt");
    CHECK_FALSE(a);
    write(dir / "later.txt", "here");
    assets.poll();
    REQUIRE(a);
    CHECK(a->value == "here");
}

TEST_CASE("assets: code-made assets") {
    AssetManager assets;
    auto a = assets.add("gen/hello", std::make_unique<Text>(Text{"hi"}));
    CHECK(a->value == "hi");
    CHECK(assets.find<Text>("gen/hello") == a);
    CHECK(assets.list().size() == 1);
}

TEST_CASE("assets: rollback drops what came after a mark and restores replaced loaders") {
    fs::path dir = temp_dir("rollback");
    write(dir / "a.txt", "a");
    AssetManager assets;
    assets.mount("", dir);
    register_text(assets);
    auto before = assets.load<Text>("a.txt");
    u64 mark = assets.mark();

    // After the mark: a replacement loader, a new asset, a callback.
    assets.register_loader<Text>([](AssetLoadContext&) { return std::make_unique<Text>(Text{"replaced"}); });
    assets.add("gen/x", std::make_unique<Text>(Text{"x"}));
    int calls = 0;
    assets.on_reload([&](std::string_view, bool) { ++calls; });
    CHECK(assets.load<Text>("b.txt")->value == "replaced");

    assets.rollback(mark);
    CHECK_FALSE(assets.find<Text>("gen/x"));
    CHECK_FALSE(assets.find<Text>("b.txt"));
    CHECK(assets.find<Text>("a.txt") == before); // made before the mark: kept
    write(dir / "c.txt", "c");
    CHECK(assets.load<Text>("c.txt")->value == "c"); // the original loader is back
    assets.reload("a.txt");
    CHECK(calls == 0); // the callback went with the rollback
}

namespace {
struct Shouted {
    std::string value;
};
} // namespace

TEST_CASE("assets: background loads read and decode on another thread, then finish on this one") {
    fs::path dir = temp_dir("async");
    write(dir / "a.txt", "hello");
    write(dir / "b.txt", "world");
    AssetManager assets;
    assets.mount("", dir);
    std::thread::id main = std::this_thread::get_id(), decoded_on;
    std::atomic<int> decodes{0};
    // The decoder does the slow part off the main thread; the loader just takes the result.
    assets.register_decoder<Shouted>([&](std::string_view, std::span<const u8> bytes) -> std::shared_ptr<void> {
        decoded_on = std::this_thread::get_id();
        ++decodes;
        std::string s(bytes.begin(), bytes.end());
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return std::make_shared<std::string>(s);
    });
    assets.register_loader<Shouted>([&](AssetLoadContext& ctx) -> std::unique_ptr<Shouted> {
        CHECK(std::this_thread::get_id() == main);
        if (auto* done = ctx.prepared<std::string>()) return std::make_unique<Shouted>(Shouted{*done});
        auto text = ctx.read_text(ctx.path());
        return text ? std::make_unique<Shouted>(Shouted{*text}) : nullptr;
    });

    Asset<Shouted> a = assets.load_async<Shouted>("a.txt");
    Asset<Shouted> b = assets.load_async<Shouted>("b.txt");
    CHECK(assets.load_async<Shouted>("a.txt") == a); // asked twice: one load
    for (int i = 0; i < 500 && assets.loading() > 0; ++i) {
        assets.finish_loads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(assets.loading() == 0);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->value == "HELLO");
    CHECK(b->value == "WORLD");
    CHECK(decodes == 2);
    CHECK(decoded_on != main);

    // load() doesn't wait: a pending asset is finished on the spot.
    write(dir / "c.txt", "now");
    Asset<Shouted> c = assets.load_async<Shouted>("c.txt");
    Asset<Shouted> c_now = assets.load<Shouted>("c.txt");
    REQUIRE(c_now);
    CHECK(c == c_now);
    CHECK(c->value == "now"); // loaded directly, without the decoder
    for (int i = 0; i < 500 && assets.loading() > 0; ++i) {
        assets.finish_loads();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(c->value == "now"); // the late background result is ignored

    // A missing file fails at once, like load().
    CHECK_FALSE(assets.load_async<Shouted>("missing.txt"));
    CHECK(assets.loading() == 0);
}

TEST_CASE("assets: archives hold a folder and mount like one") {
    fs::path src = temp_dir("archive_src"), out = temp_dir("archive_out");
    write(src / "a.txt", "alpha");
    write(src / "sub" / "b.txt", "beta");
    fs::create_directories(src / "empty");
    REQUIRE(write_archive(out / "pack.bowlpak", src));

    auto archive = Archive::open(out / "pack.bowlpak");
    REQUIRE(archive);
    CHECK(archive->names() == std::vector<std::string>{"a.txt", "sub/b.txt"});
    CHECK(archive->contains("sub/b.txt"));
    CHECK_FALSE(archive->contains("b.txt"));
    auto b = archive->read("sub/b.txt");
    REQUIRE(b);
    CHECK(std::string(b->begin(), b->end()) == "beta");
    CHECK_FALSE(archive->read("missing"));
    write(out / "not.bowlpak", "BWPK but not really");
    CHECK_FALSE(Archive::open(out / "not.bowlpak"));
    CHECK_FALSE(Archive::open(out / "missing.bowlpak"));

    // The same folder makes the same archive.
    REQUIRE(write_archive(out / "again.bowlpak", src));
    CHECK(read_file_bytes(out / "pack.bowlpak") == read_file_bytes(out / "again.bowlpak"));

    // Mounted: loads, sync and in the background; earlier mounts still win.
    fs::path loose = temp_dir("archive_loose");
    write(loose / "a.txt", "loose alpha");
    AssetManager assets;
    register_text(assets);
    REQUIRE(assets.mount_archive("data/", out / "pack.bowlpak"));
    assets.mount("data/", loose);
    CHECK_FALSE(assets.mount_archive("", out / "not.bowlpak"));
    CHECK(assets.load<Text>("data/a.txt")->value == "alpha");
    auto later = assets.load_async<Text>("data/sub/b.txt");
    for (int i = 0; i < 500 && assets.loading(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assets.finish_loads();
    }
    REQUIRE(later);
    CHECK(later->value == "beta");
    CHECK(assets.exists("data/sub/b.txt"));
    CHECK_FALSE(assets.resolve("data/sub/b.txt")); // no real file to point at
    CHECK(assets.read("data/a.txt") == std::optional<std::vector<u8>>(std::vector<u8>{'a', 'l', 'p', 'h', 'a'}));
    CHECK_FALSE(assets.exists("data/nothing.txt"));
}
