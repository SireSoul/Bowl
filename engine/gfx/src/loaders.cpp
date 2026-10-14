#include "ph/core/log.hpp"
#include "ph/gfx/device.hpp"

#if PH_RUNTIME_SHADER_COMPILER
#include "ph/shaderc/shaderc.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "ph/core/file.hpp"
#include "ph/gfx/renderer.hpp"

#include <string>

namespace ph::gfx {

namespace {

std::unique_ptr<Shader> load_shader(AssetLoadContext& ctx, GpuDevice& device) {
    std::string base(ctx.path());
#if PH_RUNTIME_SHADER_COMPILER
    // Dev builds compile the GLSL source directly, so saving a .glsl file updates the running game.
    if (auto file = ctx.assets().resolve(base + ".glsl")) {
        auto source = ctx.read_text(base + ".glsl");
        auto stage = shaderc::stage_from_filename(file->filename().string());
        if (!source || !stage) {
            log_asset_error(ctx.path(), "Shader", "unreadable source or unknown stage");
            return nullptr;
        }
        shaderc::CompileOptions options;
        options.name = file->filename().string();
        options.stage = *stage;
        options.source_path = *file;
        auto result = shaderc::compile_glsl(*source, options);
        for (const auto& inc : result.includes) ctx.depends_on(inc);
        if (!result.ok) {
            log_asset_error(ctx.path(), "Shader", result.log);
            return nullptr;
        }
        if (!result.log.empty()) warn("shader '{}': {}", ctx.path(), result.log);
        return device.create_shader(result.bundle, ctx.path());
    }
#endif
    auto bytes = ctx.read(base + ".phsh");
    if (!bytes) {
        log_asset_error(ctx.path(), "Shader", "no " + base + ".phsh (or .glsl in dev builds) in any mount");
        return nullptr;
    }
    auto bundle = ShaderBundle::parse(*bytes);
    if (!bundle) {
        log_asset_error(ctx.path(), "Shader", "corrupt or outdated .phsh bundle; rebuild shaders");
        return nullptr;
    }
    return device.create_shader(*bundle, ctx.path());
}

std::unique_ptr<Texture> load_texture(AssetLoadContext& ctx, GpuDevice& device, Filter filter) {
    auto bytes = ctx.read(ctx.path());
    if (!bytes) {
        log_asset_error(ctx.path(), "Texture", "file not found in any mount");
        return nullptr;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels, 4);
    if (!pixels) {
        log_asset_error(ctx.path(), "Texture", stbi_failure_reason());
        return nullptr;
    }
    TextureDesc desc;
    desc.width = static_cast<u32>(w);
    desc.height = static_cast<u32>(h);
    desc.rgba8 = pixels;
    desc.filter = filter;
    desc.name = ctx.path();
    auto texture = device.create_texture(desc);
    stbi_image_free(pixels);
    return texture;
}

} // namespace

bool write_png(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba) {
    std::vector<u8> png;
    auto append = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    int ok = stbi_write_png_to_func(append, &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                    static_cast<int>(width * 4));
    return ok && write_file_bytes(path, png.data(), png.size());
}

void register_asset_loaders(AssetManager& assets, GpuDevice& device, Filter default_filter) {
    assets.register_loader<Shader>([&device](AssetLoadContext& ctx) { return load_shader(ctx, device); });
    assets.register_loader<Texture>(
        [&device, default_filter](AssetLoadContext& ctx) { return load_texture(ctx, device, default_filter); });
}

} // namespace ph::gfx
