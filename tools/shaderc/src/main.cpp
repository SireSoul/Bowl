// bowl-shaderc: compiles a GLSL shader into a .bowlsh bundle (SPIR-V + MSL + HLSL).
//
//     bowl-shaderc sprite.frag.glsl -o sprite.frag.bowlsh [--depfile sprite.frag.bowlsh.d] [-g]
//                  [--print-msl|--print-hlsl] [--include <prefix>=<folder>]...

#include "bowl/core/file.hpp"
#include "bowl/shaderc/shaderc.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static std::string depfile_escape(const fs::path& p) {
    std::string out;
    for (char c : p.generic_string()) {
        if (c == ' ' || c == '#') out += '\\';
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

int main(int argc, char** argv) {
    fs::path input, output, depfile;
    bool debug = false, print_msl = false, print_hlsl = false;
    std::vector<std::pair<std::string, fs::path>> include_roots;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "--depfile" && i + 1 < argc) depfile = argv[++i];
        else if (arg == "--include" && i + 1 < argc) { // engine/shaders/=/path/to/engine/assets/shaders
            std::string_view spec = argv[++i];
            std::size_t eq = spec.find('=');
            if (eq != std::string_view::npos) include_roots.emplace_back(spec.substr(0, eq), fs::path(spec.substr(eq + 1)));
        } else if (arg == "-g") debug = true;
        else if (arg == "--print-msl") print_msl = true;
        else if (arg == "--print-hlsl") print_hlsl = true;
        else if (arg == "-h" || arg == "--help") {
            std::puts("usage: bowl-shaderc <input.(vert|frag).glsl> -o <output.bowlsh> [--depfile <file>] [-g] "
                      "[--print-msl] [--print-hlsl] [--include <prefix>=<folder>]");
            return 0;
        } else input = arg;
    }
    if (input.empty() || (output.empty() && !print_msl && !print_hlsl)) {
        std::fputs("bowl-shaderc: need an input file and -o <output>\n", stderr);
        return 2;
    }

    auto stage = bowl::shaderc::stage_from_filename(input.filename().string());
    if (!stage) {
        std::fprintf(stderr, "bowl-shaderc: can't tell the stage of '%s' (name it *.vert.glsl or *.frag.glsl)\n",
                     input.string().c_str());
        return 2;
    }
    auto source = bowl::read_file_text(input);
    if (!source) {
        std::fprintf(stderr, "bowl-shaderc: can't read '%s'\n", input.string().c_str());
        return 1;
    }

    bowl::shaderc::CompileOptions options;
    options.name = input.filename().string();
    options.stage = *stage;
    options.source_path = fs::absolute(input);
    options.debug_info = debug;
    options.include_roots = include_roots;
    auto result = bowl::shaderc::compile_glsl(*source, options);
    if (!result.log.empty()) std::fputs(result.log.c_str(), result.ok ? stdout : stderr);
    if (!result.ok) return 1;

    for (auto [wanted, format] : {std::pair{print_msl, bowl::ShaderBlobFormat::Msl}, std::pair{print_hlsl, bowl::ShaderBlobFormat::Hlsl}})
        if (wanted)
            if (auto* blob = result.bundle.find(format)) std::fwrite(blob->code.data(), 1, blob->code.size(), stdout);

    if (!output.empty()) {
        auto bytes = result.bundle.serialize();
        if (!bowl::write_file_bytes(output, bytes.data(), bytes.size())) {
            std::fprintf(stderr, "bowl-shaderc: can't write '%s'\n", output.string().c_str());
            return 1;
        }
    }
    if (!depfile.empty()) {
        std::string deps = depfile_escape(output) + ":";
        deps += " " + depfile_escape(fs::absolute(input));
        for (const auto& inc : result.includes) deps += " " + depfile_escape(fs::absolute(inc));
        deps += "\n";
        bowl::write_file_bytes(depfile, deps.data(), deps.size());
    }
    return 0;
}
