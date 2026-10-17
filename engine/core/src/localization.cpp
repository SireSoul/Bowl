#include "bowl/core/localization.hpp"

#include "bowl/core/json.hpp"
#include "bowl/core/log.hpp"

#include <format>
#include <span>

namespace bowl {

const std::string* StringTable::find(std::string_view key) const {
    auto it = strings.find(std::string(key));
    return it == strings.end() ? nullptr : &it->second;
}

static void flatten(const json::Value& v, const std::string& prefix, StringTable& out) {
    for (const auto& [key, child] : v.object()) {
        std::string full = prefix.empty() ? key : prefix + "." + key;
        if (child.is_object()) flatten(child, full, out);
        else if (child.is_string()) out.strings[full] = std::string(child.as_string());
        else if (child.is_number()) out.strings[full] = json::stringify(child, -1);
    }
}

bool StringTable::parse(std::string_view json_text, StringTable& out, std::string& error) {
    // Text, or the binary JSON bowl-cook makes of it.
    auto bytes = std::span(reinterpret_cast<const u8*>(json_text.data()), json_text.size());
    auto parsed = json::parse_any(bytes);
    if (!parsed) {
        error = json::is_binary(bytes)
                    ? std::format("byte {}: {}", parsed.error->column, parsed.error->message)
                    : std::format("{}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message);
        return false;
    }
    if (!parsed.value.is_object()) {
        error = "a string table must be a JSON object";
        return false;
    }
    flatten(parsed.value, "", out);
    return true;
}

void register_string_table_loader(AssetManager& assets) {
    assets.register_loader<StringTable>([](AssetLoadContext& ctx) -> std::unique_ptr<StringTable> {
        auto text = ctx.read_text(ctx.path());
        if (!text) {
            log_asset_error(ctx.path(), "StringTable", "file not found in any mount");
            return nullptr;
        }
        auto table = std::make_unique<StringTable>();
        std::string error;
        if (!StringTable::parse(*text, *table, error)) {
            log_asset_error(ctx.path(), "StringTable", error);
            return nullptr;
        }
        return table;
    });
}

void Localization::set_language(std::string_view code) {
    language_ = std::string(code);
    std::string path = "lang/" + language_ + ".json";
    current_ = assets_->resolve(path) ? assets_->load<StringTable>(path) : Asset<StringTable>{};
    if (!current_) warn("localization: no strings for '{}' ({} not found)", language_, path);
}

void Localization::set_fallback(std::string_view code) {
    fallback_ = std::string(code);
    std::string path = "lang/" + fallback_ + ".json";
    fallback_table_ = assets_->resolve(path) ? assets_->load<StringTable>(path) : Asset<StringTable>{};
}

std::string_view Localization::tr(std::string_view key) const {
    if (current_)
        if (const std::string* s = current_->find(key)) return *s;
    if (fallback_table_)
        if (const std::string* s = fallback_table_->find(key)) return *s;
    return key;
}

bool Localization::has(std::string_view key) const {
    return (current_ && current_->find(key)) || (fallback_table_ && fallback_table_->find(key));
}

std::string fill_placeholders(std::string_view text,
                              std::initializer_list<std::pair<std::string_view, std::string_view>> args) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size();) {
        if (text[i] == '{') {
            usize close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                std::string_view name = text.substr(i + 1, close - i - 1);
                bool replaced = false;
                for (const auto& [k, v] : args)
                    if (k == name) {
                        out += v;
                        replaced = true;
                        break;
                    }
                if (replaced) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

std::string Localization::format(std::string_view key,
                                 std::initializer_list<std::pair<std::string_view, std::string_view>> args) const {
    return fill_placeholders(tr(key), args);
}

std::string Localization::plural(std::string_view key, i64 count) const {
    // Simple one/other rule (English, Spanish, German, ...). Languages with more forms can add their own keys.
    std::string form = std::string(key) + (count == 1 ? ".one" : ".other");
    if (!has(form)) form = std::string(key);
    std::string n = std::to_string(count);
    return fill_placeholders(tr(form), {{"n", n}});
}

} // namespace bowl
