#include "bowl/core/file.hpp"
#include "bowl/core/localization.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace bowl;
namespace fs = std::filesystem;

namespace {
void write(const fs::path& p, std::string_view s) { REQUIRE(write_file_bytes(p, s.data(), s.size())); }
} // namespace

TEST_CASE("localization: lookup, fallback, placeholders, plurals") {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / "lang_root";
    fs::remove_all(dir);
    write(dir / "lang" / "en.json", R"({
        "menu": { "start": "Start", "quit": "Quit" },
        "greeting": "Hello, {name}!",
        "coins": { "one": "{n} coin", "other": "{n} coins" },
        "only_english": "Only in English"
    })");
    write(dir / "lang" / "es.json", R"({
        "menu": { "start": "Empezar", "quit": "Salir" },
        "greeting": "¡Hola, {name}!",
        "coins": { "one": "{n} moneda", "other": "{n} monedas" }
    })");

    AssetManager assets;
    assets.mount("", dir);
    register_string_table_loader(assets);
    Localization i18n(assets);
    i18n.set_fallback("en");
    i18n.set_language("es");

    CHECK(i18n.tr("menu.start") == "Empezar");
    CHECK(i18n.tr("only_english") == "Only in English"); // fallback
    CHECK(i18n.tr("missing.key") == "missing.key");       // visible gap
    CHECK(i18n.format("greeting", {{"name", "Ada"}}) == "¡Hola, Ada!");
    CHECK(i18n.plural("coins", 1) == "1 moneda");
    CHECK(i18n.plural("coins", 5) == "5 monedas");

    i18n.set_language("en");
    CHECK(i18n.tr("menu.quit") == "Quit");
    CHECK(i18n.plural("coins", 0) == "0 coins");
}

TEST_CASE("localization: placeholders leave unknown names alone") {
    CHECK(fill_placeholders("{a} and {b} and {", {{"a", "1"}}) == "1 and {b} and {");
}

TEST_CASE("localization: bad JSON is reported") {
    StringTable t;
    std::string error;
    CHECK_FALSE(StringTable::parse("{ \"a\": ", t, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(StringTable::parse("[1, 2]", t, error));
}

TEST_CASE("localization: a string from tr() outlives a reload of its table") {
    fs::path dir = fs::temp_directory_path() / "bowl_tests" / "lang_reload";
    fs::remove_all(dir);
    write(dir / "lang" / "en.json", R"({ "title": "A title long enough not to fit in a short string" })");
    AssetManager assets;
    assets.mount("", dir);
    register_string_table_loader(assets);
    Localization i18n(assets);
    i18n.set_language("en");
    std::string_view title = i18n.tr("title");
    write(dir / "lang" / "en.json", R"({ "title": "Another title, also too long for a short string" })");
    REQUIRE(assets.reload("lang/en.json")); // the old table is freed
    CHECK(title == "A title long enough not to fit in a short string");
    CHECK(i18n.tr("title") == "Another title, also too long for a short string");
    CHECK(i18n.tr("title").data() == i18n.tr("title").data()); // the same string twice isn't kept twice
}
