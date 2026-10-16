// bowl-pack: puts a folder into an asset archive (.bowlpak), or lists what's in one.
//
//     bowl-pack assets -o assets.bowlpak
//     bowl-pack --list assets.bowlpak
//
// A shipping build with BOWL_PACK_ASSETS runs it for you: next to the game, engine.bowlpak and assets.bowlpak replace
// the engine/ and assets/ folders, and the engine reads from them.

#include "bowl/core/archive.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--list") {
        auto archive = bowl::Archive::open(argv[2]);
        if (!archive) {
            std::fprintf(stderr, "bowl-pack: %s isn't an asset archive\n", argv[2]);
            return 1;
        }
        for (const auto& name : archive->names()) std::printf("%s\n", name.c_str());
        return 0;
    }
    if (argc != 4 || std::string(argv[2]) != "-o") {
        std::fprintf(stderr,
                     "usage: bowl-pack <folder> -o <archive.bowlpak>\n       bowl-pack --list <archive.bowlpak>\n");
        return 2;
    }
    if (!bowl::write_archive(argv[3], argv[1])) {
        std::fprintf(stderr, "bowl-pack: couldn't pack %s into %s\n", argv[1], argv[3]);
        return 1;
    }
    return 0;
}
