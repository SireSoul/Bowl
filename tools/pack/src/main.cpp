// ph-pack: puts a folder into an asset archive (.phpak), or lists what's in one.
//
//     ph-pack assets -o assets.phpak
//     ph-pack --list assets.phpak
//
// A shipping build with PH_PACK_ASSETS runs it for you: next to the game, engine.phpak and assets.phpak replace the
// engine/ and assets/ folders, and the engine reads from them.

#include "ph/core/archive.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--list") {
        auto archive = ph::Archive::open(argv[2]);
        if (!archive) {
            std::fprintf(stderr, "ph-pack: %s isn't an asset archive\n", argv[2]);
            return 1;
        }
        for (const auto& name : archive->names()) std::printf("%s\n", name.c_str());
        return 0;
    }
    if (argc != 4 || std::string(argv[2]) != "-o") {
        std::fprintf(stderr, "usage: ph-pack <folder> -o <archive.phpak>\n       ph-pack --list <archive.phpak>\n");
        return 2;
    }
    if (!ph::write_archive(argv[3], argv[1])) {
        std::fprintf(stderr, "ph-pack: couldn't pack %s into %s\n", argv[1], argv[3]);
        return 1;
    }
    return 0;
}
