#pragma once

#include "bowl/core/types.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Cooking: turns an asset folder into what a shipping build loads fastest, file by file, keeping every path, so a game
// loads its cooked assets with the same calls. Only files whose loaders the engine owns change:
//   .world, .prefab, lang/*.json   binary JSON (json::encode_binary)
//   .tmj, .tmx                     one binary-JSON map with its tilesets and object templates in it, and its layer data
//                                  as plain arrays; the .tsj, .tsx, .tj and .tx files put into maps are left out
//   .png, .jpg, .jpeg              BWTX pictures (gfx/bwtx.hpp)
// Everything else is copied as it is. Cooking is deterministic (the same folder always gives the same bytes), and
// cooking a cooked folder changes nothing. Used by the bowl-cook tool, which a BOWL_PACK_ASSETS build runs before
// packing.
namespace bowl::cook {

enum class Kind : u8 { Copy, Json, Map, Picture };

// What cooking does with a file at this path (relative to the folder, with '/'), by its name alone.
Kind kind_of(std::string_view path);

struct Cooked {
    std::vector<u8> bytes;
    std::vector<std::string> inlined; // other files it now holds (a map's tilesets and templates)
};

// The cooked form of one file; nothing (with `error` empty) to copy it as it is, or with `error` set if it's broken.
// `read` gets the other files a map names, by their normalized path in the folder.
std::optional<Cooked> cook_file(std::string_view path, std::span<const u8> bytes,
                                const std::function<std::optional<std::string>(const std::string& path)>& read,
                                std::string& error);

struct Report {
    usize cooked = 0, copied = 0, left_out = 0;
    u64 bytes_in = 0, bytes_out = 0;
    std::vector<std::string> errors; // "path: what's wrong"
};

// Cooks every file under `in` into `out`, which may be `in` itself (cooking in place). False if any file was broken
// or couldn't be written; the others are still cooked.
bool cook_folder(const std::filesystem::path& in, const std::filesystem::path& out, Report& report);

} // namespace bowl::cook
