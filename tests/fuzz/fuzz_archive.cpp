// Asset archives (.bowlpak): a game's assets, which players can swap for their own.

#include "fuzz.hpp"

#include "bowl/core/archive.hpp"
#include "bowl/core/file.hpp"

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace bowl;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Archives are read from files; one per process, so fuzzers running side by side (-jobs) don't share it.
    static const struct TempFile {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("bowl_fuzz_" + std::to_string(getpid()) + ".bowlpak");
        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } file;
    const std::filesystem::path& path = file.path;
    if (!write_file_bytes(path, data, size)) return 0;
    auto archive = Archive::open(path);
    if (!archive) return 0;
    for (const std::string& name : archive->names()) {
        BOWL_FUZZ_CHECK(archive->contains(name));
        auto bytes = archive->read(name);
        BOWL_FUZZ_CHECK(bytes && bytes->size() <= size);
    }
    return 0;
}
