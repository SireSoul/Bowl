// bowl-icon: application icons from one picture.
//
//     bowl-icon icon.png [--icns out.icns] [--ico out.ico] [--png <size> out.png] [--cpp out.cpp] [--pixel-art]
//
// Give it a square PNG, 1024 x 1024 at best (smaller ones are scaled up; --pixel-art keeps them crisp). bowl_add_game's
// ICON runs it for you.

#include "bowl/core/file.hpp"
#include "bowl/icon/icon.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <vector>

using namespace bowl;

namespace {

bool write(const std::string& path, std::span<const u8> bytes) {
    if (write_file_bytes(path, bytes.data(), bytes.size())) return true;
    std::fprintf(stderr, "bowl-icon: can't write %s\n", path.c_str());
    return false;
}

int usage() {
    std::fprintf(stderr, "usage: bowl-icon icon.png [--icns out.icns] [--ico out.ico] [--png <size> out.png] [--cpp out.cpp] "
                         "[--pixel-art]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string input = argv[1];
    std::string icns, ico, cpp;
    std::vector<std::pair<u32, std::string>> pngs;
    bool pixel_art = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--icns" && i + 1 < argc) icns = argv[++i];
        else if (arg == "--ico" && i + 1 < argc) ico = argv[++i];
        else if (arg == "--cpp" && i + 1 < argc) cpp = argv[++i];
        else if (arg == "--pixel-art") pixel_art = true;
        else if (arg == "--png" && i + 2 < argc) {
            u32 size = 0;
            std::string_view s = argv[++i];
            if (std::from_chars(s.data(), s.data() + s.size(), size).ec != std::errc{} || size == 0 || size > 4096)
                return usage();
            pngs.emplace_back(size, argv[++i]);
        } else {
            return usage();
        }
    }

    auto bytes = read_file_bytes(input);
    if (!bytes) {
        std::fprintf(stderr, "bowl-icon: can't read %s\n", input.c_str());
        return 1;
    }
    auto image = icon::decode_png(*bytes);
    if (!image) {
        std::fprintf(stderr, "bowl-icon: %s isn't a PNG\n", input.c_str());
        return 1;
    }
    if (image->width != image->height)
        std::fprintf(stderr, "bowl-icon: %s isn't square (%ux%u); centering it\n", input.c_str(), image->width, image->height);

    bool ok = true;
    if (!icns.empty()) ok &= write(icns, icon::make_icns(*image, pixel_art));
    if (!ico.empty()) ok &= write(ico, icon::make_ico(*image, pixel_art));
    for (const auto& [size, path] : pngs) ok &= write(path, icon::encode_png(icon::resize(*image, size, pixel_art)));
    if (!cpp.empty()) {
        std::string source = icon::make_cpp(icon::encode_png(icon::resize(*image, 256, pixel_art)));
        ok &= write(cpp, std::span(reinterpret_cast<const u8*>(source.data()), source.size()));
    }
    return ok ? 0 : 1;
}
