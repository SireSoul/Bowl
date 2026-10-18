#pragma once

#include "bowl/core/assets.hpp"

#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bowl {

// One language's strings, from a JSON file (assets "lang/<code>.json"). Nested objects become dotted keys:
//     { "menu": { "start": "Start", "quit": "Quit" }, "coins": { "one": "{n} coin", "other": "{n} coins" } }
class StringTable {
public:
    std::unordered_map<std::string, std::string> strings;
    const std::string* find(std::string_view key) const;

    // `json_text` may also be the binary JSON bowl-cook makes of the file.
    static bool parse(std::string_view json_text, StringTable& out, std::string& error);
};

// Looks strings up in the current language, then the fallback, then shows the key itself so gaps are visible.
// Tables hot-reload like any asset.
//
//     i18n.set_language("es");
//     draw_text(..., i18n.tr("menu.start"));
//     i18n.format("greeting", {{"name", player}});      // "Hello, {name}!"
//     i18n.plural("coins", 3);                           // "coins.one" / "coins.other", with {n} filled in
class Localization {
public:
    explicit Localization(AssetManager& assets) : assets_(&assets) {}

    void set_language(std::string_view code);
    void set_fallback(std::string_view code);
    const std::string& language() const { return language_; }

    // The string stays valid as long as this Localization does, even after the table reloads or the language changes
    // (only `key` itself, given back when nothing has it, lives as long as the caller's).
    std::string_view tr(std::string_view key) const;
    std::string format(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> args) const;
    std::string plural(std::string_view key, i64 count) const;
    bool has(std::string_view key) const;

private:
    AssetManager* assets_;
    std::string language_, fallback_;
    Asset<StringTable> current_, fallback_table_;
    // Every string tr() has handed out, kept: a table that reloads frees its own.
    struct TextHash {
        using is_transparent = void;
        usize operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    mutable std::unordered_set<std::string, TextHash, std::equal_to<>> handed_out_;
    mutable std::mutex handed_out_mutex_;
    std::string_view keep(const std::string& s) const;
};

// Replaces {name} placeholders. Unknown placeholders are left as they are.
std::string fill_placeholders(std::string_view text, std::initializer_list<std::pair<std::string_view, std::string_view>> args);

void register_string_table_loader(AssetManager& assets);

} // namespace bowl
