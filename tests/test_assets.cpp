#include "ph/core/assets.hpp"
#include "ph/core/file.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace ph;
namespace fs = std::filesystem;

namespace {

struct Text {
    std::string value;
};

fs::path temp_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() / "ph_tests" / name;
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
