#include "bowl/cook/cook.hpp"

#include "bowl/core/file.hpp"
#include "bowl/core/json.hpp"
#include "bowl/gfx/bwtx.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace bowl::cook {

namespace {

std::string lower_extension(std::string_view path) {
    usize slash = path.rfind('/');
    usize dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string ext(path.substr(dot));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string_view as_text(std::span<const u8> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

Kind kind_of(std::string_view path) {
    std::string ext = lower_extension(path);
    if (ext == ".world" || ext == ".prefab") return Kind::Json;
    // Localization reads "lang/<code>.json" from the top of the assets.
    if (ext == ".json" && path.starts_with("lang/") && path.find('/', 5) == std::string_view::npos) return Kind::Json;
    if (ext == ".tmj" || ext == ".tmx") return Kind::Map;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") return Kind::Picture;
    return Kind::Copy;
}

std::optional<Cooked> cook_file(std::string_view path, std::span<const u8> bytes,
                                const std::function<std::optional<std::string>(const std::string&)>& read,
                                std::string& error) {
    error.clear();
    switch (kind_of(path)) {
    case Kind::Copy:
        return std::nullopt;
    case Kind::Json: {
        if (json::is_binary(bytes)) return std::nullopt; // cooked already
        auto parsed = json::parse(as_text(bytes));
        if (!parsed) {
            error = std::format("{}:{}: {}", parsed.error->line, parsed.error->column, parsed.error->message);
            return std::nullopt;
        }
        return Cooked{json::encode_binary(parsed.value), {}};
    }
    case Kind::Map: {
        if (json::is_binary(bytes)) return std::nullopt;
        Cooked cooked;
        auto doc = gfx::Tilemap::flatten(as_text(bytes), path, read, cooked.inlined, error);
        if (!doc) return std::nullopt;
        // What the game will load must load: check it the way the loader does, with no other file to read.
        gfx::Tilemap map;
        auto none = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
        usize slash = path.rfind('/');
        std::string dir(slash == std::string_view::npos ? std::string_view() : path.substr(0, slash + 1));
        if (!gfx::Tilemap::parse(*doc, dir, none, map, error)) return std::nullopt;
        cooked.bytes = json::encode_binary(*doc);
        return cooked;
    }
    case Kind::Picture: {
        if (gfx::is_bwtx(bytes)) return std::nullopt;
        u32 w = 0, h = 0;
        std::vector<u8> rgba;
        if (!gfx::decode_png(bytes, w, h, rgba)) {
            error = "not a picture the engine can read";
            return std::nullopt;
        }
        if (w > gfx::bwtx_max_side || h > gfx::bwtx_max_side) {
            error = std::format("{} x {} is bigger than a texture can be ({} a side)", w, h, gfx::bwtx_max_side);
            return std::nullopt;
        }
        return Cooked{gfx::encode_bwtx(w, h, rgba), {}};
    }
    }
    return std::nullopt;
}

bool cook_folder(const fs::path& in, const fs::path& out, Report& report) {
    std::error_code ec;
    if (!fs::is_directory(in, ec)) {
        report.errors.push_back(std::format("{}: not a folder", in.string()));
        return false;
    }
    bool in_place = fs::weakly_canonical(in, ec) == fs::weakly_canonical(out, ec);

    // Every file, in path order, so the same folder always cooks the same way.
    std::vector<std::string> paths;
    for (auto it = fs::recursive_directory_iterator(in, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) paths.push_back(fs::relative(it->path(), in, ec).generic_string());
    if (ec) {
        report.errors.push_back(std::format("{}: {}", in.string(), ec.message()));
        return false;
    }
    std::sort(paths.begin(), paths.end());

    auto read = [&](const std::string& path) -> std::optional<std::string> {
        if (path.empty() || path.starts_with("../") || path == ".." || path.starts_with('/')) return std::nullopt;
        return read_file_text(in / path);
    };
    auto fail = [&](const std::string& path, std::string_view why) {
        report.errors.push_back(std::format("{}: {}", path, why));
    };

    // Maps first: the tilesets and templates they take in are left out of the folder.
    std::vector<std::pair<std::string, std::vector<u8>>> maps;
    std::set<std::string> inlined;
    for (const std::string& path : paths) {
        if (kind_of(path) != Kind::Map) continue;
        auto bytes = read_file_bytes(in / path);
        if (!bytes) {
            fail(path, "can't read it");
            continue;
        }
        std::string error;
        auto cooked = cook_file(path, *bytes, read, error);
        if (!error.empty()) fail(path, error);
        if (!cooked) continue;
        for (const std::string& file : cooked->inlined) {
            std::string ext = lower_extension(file);
            if (ext == ".tsj" || ext == ".tsx" || ext == ".tj" || ext == ".tx") inlined.insert(file);
        }
        maps.emplace_back(path, std::move(cooked->bytes));
    }
    if (!report.errors.empty()) inlined.clear(); // a broken map still needs them

    for (const std::string& path : paths) {
        fs::path from = in / path, to = out / path;
        u64 size = fs::file_size(from, ec);
        if (ec) size = 0;
        report.bytes_in += size;
        if (inlined.contains(path)) {
            if (in_place) fs::remove(to, ec);
            ++report.left_out;
            continue;
        }
        std::optional<std::vector<u8>> cooked;
        auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& m) { return m.first == path; });
        if (map != maps.end()) {
            cooked = std::move(map->second);
        } else if (kind_of(path) != Kind::Copy && kind_of(path) != Kind::Map) {
            auto bytes = read_file_bytes(from);
            if (!bytes) {
                fail(path, "can't read it");
            } else {
                std::string error;
                if (auto c = cook_file(path, *bytes, read, error)) cooked = std::move(c->bytes);
                if (!error.empty()) fail(path, error);
            }
        }
        fs::create_directories(to.parent_path(), ec);
        if (cooked) {
            ++report.cooked;
            report.bytes_out += cooked->size();
            if (!write_file_bytes(to, cooked->data(), cooked->size())) fail(path, "can't write it");
            continue;
        }
        ++report.copied;
        report.bytes_out += size;
        if (!in_place && !fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec))
            fail(path, std::format("can't copy it: {}", ec.message()));
    }
    return report.errors.empty();
}

} // namespace bowl::cook
