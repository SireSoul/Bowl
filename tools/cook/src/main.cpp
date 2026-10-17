// bowl-cook: turns an asset folder into what a shipping build loads fastest, keeping every file's path (see
// bowl/cook/cook.hpp for what changes).
//
//     bowl-cook assets cooked/assets
//     bowl-cook build/game/assets build/game/assets     (in place)
//
// A build with BOWL_PACK_ASSETS (and BOWL_COOK_ASSETS, on with it) runs it on each copied folder before bowl-pack.

#include "bowl/cook/cook.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: bowl-cook <folder> <cooked folder>   (the same folder twice cooks in place)\n");
        return 2;
    }
    bowl::cook::Report report;
    bool ok = bowl::cook::cook_folder(argv[1], argv[2], report);
    for (const auto& error : report.errors) std::fprintf(stderr, "bowl-cook: %s\n", error.c_str());
    std::printf("bowl-cook: %s: %zu cooked, %zu copied, %zu put into maps; %.1f KB -> %.1f KB\n", argv[1],
                report.cooked, report.copied, report.left_out, static_cast<double>(report.bytes_in) / 1024.0,
                static_cast<double>(report.bytes_out) / 1024.0);
    return ok ? 0 : 1;
}
