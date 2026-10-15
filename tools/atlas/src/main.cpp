// ph-atlas: packs a folder of PNGs into one atlas image plus sprite-sheet JSON that gfx::SpriteSheet loads.
//
//     ph-atlas art/characters -o assets/characters.png [--padding 1] [--max 4096] [--no-trim] [--fps 10]
//
// Writes characters.png and characters.json next to each other. Frames are named after the files (without
// extension, with their subfolder: "npc/baker"); files named walk_0.png, walk_1.png... also become a "walk" animation.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include "ph/atlas/atlas.hpp"
#include "ph/core/file.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// "walk_2" before "walk_10": digit runs compare as numbers.
bool natural_less(const std::string& a, const std::string& b) {
    using ph::usize;
    usize i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            usize i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            unsigned long long x = 0, y = 0;
            std::from_chars(a.data() + i, a.data() + i2, x);
            std::from_chars(b.data() + j, b.data() + j2, y);
            if (x != y) return x < y;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

int usage() {
    std::puts("usage: ph-atlas <folder or images...> -o <atlas.png> [--padding N] [--max N] [--no-trim] [--fps N]");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    fs::path output;
    ph::atlas::Options options;
    unsigned fps = 10;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto number = [&](unsigned& out) {
            if (i + 1 >= argc) return false;
            std::string_view v = argv[++i];
            return std::from_chars(v.data(), v.data() + v.size(), out).ec == std::errc{};
        };
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "--padding") {
            if (!number(options.padding)) return usage();
        } else if (a == "--max") {
            if (!number(options.max_size)) return usage();
        } else if (a == "--fps") {
            if (!number(fps) || fps == 0) return usage();
        } else if (a == "--no-trim") options.trim = false;
        else if (a == "-h" || a == "--help") return usage();
        else inputs.emplace_back(a);
    }
    if (inputs.empty() || output.empty()) return usage();

    // Collect PNGs, named by their path relative to the folder given.
    std::vector<std::pair<std::string, fs::path>> files;
    for (const fs::path& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (auto it = fs::recursive_directory_iterator(in, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file() || it->path().extension() != ".png") continue;
                fs::path rel = fs::relative(it->path(), in);
                rel.replace_extension();
                files.emplace_back(rel.generic_string(), it->path());
            }
        } else if (fs::is_regular_file(in, ec)) {
            files.emplace_back(in.stem().string(), in);
        } else {
            std::fprintf(stderr, "ph-atlas: can't read %s\n", in.string().c_str());
            return 1;
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& x, const auto& y) { return natural_less(x.first, y.first); });

    std::vector<ph::atlas::Image> images;
    for (const auto& [name, path] : files) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
        if (!px) {
            std::fprintf(stderr, "ph-atlas: %s: %s\n", path.string().c_str(), stbi_failure_reason());
            return 1;
        }
        images.push_back({name, static_cast<ph::u32>(w), static_cast<ph::u32>(h),
                          std::vector<ph::u8>(px, px + static_cast<size_t>(w) * h * 4)});
        stbi_image_free(px);
    }

    std::string error;
    auto atlas = ph::atlas::pack(images, options, error);
    if (!atlas) {
        std::fprintf(stderr, "ph-atlas: %s\n", error.c_str());
        return 1;
    }
    if (!stbi_write_png(output.string().c_str(), static_cast<int>(atlas->width), static_cast<int>(atlas->height), 4,
                        atlas->rgba.data(), static_cast<int>(atlas->width * 4))) {
        std::fprintf(stderr, "ph-atlas: can't write %s\n", output.string().c_str());
        return 1;
    }
    fs::path json_path = output;
    json_path.replace_extension(".json");
    std::string text = ph::json::stringify(ph::atlas::sheet_json(*atlas, images, output.filename().string(), 1000 / fps));
    if (!ph::write_file_bytes(json_path, text.data(), text.size())) {
        std::fprintf(stderr, "ph-atlas: can't write %s\n", json_path.string().c_str());
        return 1;
    }
    std::printf("ph-atlas: %zu pictures -> %s (%u x %u) and %s\n", images.size(), output.string().c_str(), atlas->width,
                atlas->height, json_path.filename().string().c_str());
    return 0;
}
