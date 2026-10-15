// ph-shaderc: compiles a GLSL shader into a .phsh bundle (SPIR-V + MSL + HLSL).
//
//     ph-shaderc sprite.frag.glsl -o sprite.frag.phsh [--depfile sprite.frag.phsh.d] [-g] [--print-msl|--print-hlsl]

#include "ph/core/file.hpp"
#include "ph/shaderc/shaderc.hpp"

#include <cstdio>
#include <string>
#include <string_view>

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
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "--depfile" && i + 1 < argc) depfile = argv[++i];
        else if (arg == "-g") debug = true;
        else if (arg == "--print-msl") print_msl = true;
        else if (arg == "--print-hlsl") print_hlsl = true;
        else if (arg == "-h" || arg == "--help") {
            std::puts("usage: ph-shaderc <input.(vert|frag).glsl> -o <output.phsh> [--depfile <file>] [-g] "
                      "[--print-msl] [--print-hlsl]");
            return 0;
        } else input = arg;
    }
    if (input.empty() || (output.empty() && !print_msl && !print_hlsl)) {
        std::fputs("ph-shaderc: need an input file and -o <output>\n", stderr);
        return 2;
    }

    auto stage = ph::shaderc::stage_from_filename(input.filename().string());
    if (!stage) {
        std::fprintf(stderr, "ph-shaderc: can't tell the stage of '%s' (name it *.vert.glsl or *.frag.glsl)\n",
                     input.string().c_str());
        return 2;
    }
    auto source = ph::read_file_text(input);
    if (!source) {
        std::fprintf(stderr, "ph-shaderc: can't read '%s'\n", input.string().c_str());
        return 1;
    }

    ph::shaderc::CompileOptions options;
    options.name = input.filename().string();
    options.stage = *stage;
    options.source_path = fs::absolute(input);
    options.debug_info = debug;
    auto result = ph::shaderc::compile_glsl(*source, options);
    if (!result.log.empty()) std::fputs(result.log.c_str(), result.ok ? stdout : stderr);
    if (!result.ok) return 1;

    for (auto [wanted, format] : {std::pair{print_msl, ph::ShaderBlobFormat::Msl}, std::pair{print_hlsl, ph::ShaderBlobFormat::Hlsl}})
        if (wanted)
            if (auto* blob = result.bundle.find(format)) std::fwrite(blob->code.data(), 1, blob->code.size(), stdout);

    if (!output.empty()) {
        auto bytes = result.bundle.serialize();
        if (!ph::write_file_bytes(output, bytes.data(), bytes.size())) {
            std::fprintf(stderr, "ph-shaderc: can't write '%s'\n", output.string().c_str());
            return 1;
        }
    }
    if (!depfile.empty()) {
        std::string deps = depfile_escape(output) + ":";
        deps += " " + depfile_escape(fs::absolute(input));
        for (const auto& inc : result.includes) deps += " " + depfile_escape(fs::absolute(inc));
        deps += "\n";
        ph::write_file_bytes(depfile, deps.data(), deps.size());
    }
    return 0;
}
