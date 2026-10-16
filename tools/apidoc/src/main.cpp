// bowl-apidoc: the API reference from the engine's public headers.
//
//     bowl-apidoc <engine folder> <output folder>
//
// Writes <module>.md for each engine/<module>/include/bowl/<module>/ and README.md, an index of them all. Files whose
// contents haven't changed are left alone. The bowl_api_docs target runs it into docs/reference/api.

#include "bowl/apidoc/apidoc.hpp"
#include "bowl/core/file.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bowl;

namespace {

bool write_if_changed(const fs::path& path, const std::string& text) {
    if (auto old = read_file_bytes(path); old && std::string(old->begin(), old->end()) == text) return true;
    if (write_file_bytes(path, text.data(), text.size())) {
        std::printf("bowl-apidoc: wrote %s\n", path.string().c_str());
        return true;
    }
    std::fprintf(stderr, "bowl-apidoc: can't write %s\n", path.string().c_str());
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: bowl-apidoc <engine folder> <output folder>\n");
        return 2;
    }
    fs::path engine = argv[1], out = argv[2];
    std::error_code ec;
    std::vector<std::pair<std::string, std::vector<apidoc::Header>>> modules;
    std::vector<fs::path> module_dirs;
    for (const auto& entry : fs::directory_iterator(engine, ec))
        if (entry.is_directory()) module_dirs.push_back(entry.path());
    std::sort(module_dirs.begin(), module_dirs.end());
    for (const fs::path& dir : module_dirs) {
        std::string module = dir.filename().string();
        fs::path include = dir / "include" / "bowl" / module;
        if (!fs::is_directory(include, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(include, ec))
            if (entry.path().extension() == ".hpp") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        std::vector<apidoc::Header> headers;
        for (const fs::path& file : files) {
            auto bytes = read_file_bytes(file);
            if (!bytes) {
                std::fprintf(stderr, "bowl-apidoc: can't read %s\n", file.string().c_str());
                return 1;
            }
            headers.push_back(apidoc::parse("bowl/" + module + "/" + file.filename().string(),
                                            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size())));
        }
        if (!headers.empty()) modules.emplace_back(module, std::move(headers));
    }
    if (modules.empty()) {
        std::fprintf(stderr, "bowl-apidoc: no headers under %s/<module>/include/bowl/<module>/\n",
                     engine.string().c_str());
        return 1;
    }
    fs::create_directories(out, ec);
    bool ok = true;
    for (const auto& [module, headers] : modules) ok &= write_if_changed(out / (module + ".md"), apidoc::render_module(module, headers) + "\n");
    ok &= write_if_changed(out / "README.md", apidoc::render_index(modules));
    return ok ? 0 : 1;
}
