#include "bowl/core/log.hpp"
#include "bowl/gfx/device.hpp"

#if BOWL_RUNTIME_SHADER_COMPILER
#include "bowl/shaderc/shaderc.hpp"
#endif

#define STBI_NO_STDIO
#include <stb_image.h> // declarations only: implemented in stb_image.cpp
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include "bowl/core/file.hpp"
#include "bowl/gfx/bwtx.hpp"
#include "bowl/gfx/renderer.hpp"
#include "bowl/gfx/sprite_sheet.hpp"
#include "bowl/gfx/text.hpp"
#include "bowl/gfx/tilemap.hpp"

#include <climits>
#include <string>

namespace bowl::gfx {

namespace {

// The bundle for "<path>": compiled from "<path>.glsl" in dev builds, read from "<path>.bowlsh" otherwise.
std::optional<ShaderBundle> load_bundle(AssetLoadContext& ctx, std::string_view type) {
    std::string base(ctx.path());
#if BOWL_RUNTIME_SHADER_COMPILER
    // Dev builds compile the GLSL source directly, so saving a .glsl file updates the running game.
    if (auto file = ctx.assets().resolve(base + ".glsl")) {
        auto source = ctx.read_text(base + ".glsl");
        auto stage = shaderc::stage_from_filename(file->filename().string());
        if (!source || !stage) {
            log_asset_error(ctx.path(), type, "unreadable source or unknown stage");
            return std::nullopt;
        }
        shaderc::CompileOptions options;
        options.name = file->filename().string();
        options.stage = *stage;
        options.source_path = *file;
        // Game shaders can #include "engine/shaders/...": wherever the engine's shaders are mounted.
        if (auto engine_shaders = ctx.assets().resolve("engine/shaders/common.glsl"))
            options.include_roots.emplace_back("engine/shaders/", engine_shaders->parent_path());
        auto result = shaderc::compile_glsl(*source, options);
        for (const auto& inc : result.includes) ctx.depends_on(inc);
        if (!result.ok) {
            log_asset_error(ctx.path(), type, result.log);
            return std::nullopt;
        }
        if (!result.log.empty()) warn("shader '{}': {}", ctx.path(), result.log);
        return std::move(result.bundle);
    }
#endif
    auto bytes = ctx.read(base + ".bowlsh");
    if (!bytes) {
        log_asset_error(ctx.path(), type, "no " + base + ".bowlsh (or .glsl in dev builds) in any mount");
        return std::nullopt;
    }
    auto bundle = ShaderBundle::parse(*bytes);
    if (!bundle) log_asset_error(ctx.path(), type, "corrupt or outdated .bowlsh bundle; rebuild shaders");
    return bundle;
}

std::unique_ptr<Shader> load_shader(AssetLoadContext& ctx, GpuDevice& device) {
    auto bundle = load_bundle(ctx, "Shader");
    return bundle ? device.create_shader(*bundle, ctx.path()) : nullptr;
}

std::unique_ptr<ComputeShader> load_compute_shader(AssetLoadContext& ctx, GpuDevice& device) {
    auto bundle = load_bundle(ctx, "ComputeShader");
    return bundle ? device.create_compute_shader(*bundle, ctx.path()) : nullptr;
}

// A picture decoded to RGBA (on the loading thread, for background loads).
struct DecodedImage {
    int w = 0, h = 0;
    u8* pixels = nullptr; // stb's, or into `cooked`
    std::vector<u8> cooked;
    const char* error = nullptr;
    ~DecodedImage() {
        if (pixels && cooked.empty()) stbi_image_free(pixels);
    }
};

std::shared_ptr<DecodedImage> decode_image(std::span<const u8> bytes) {
    auto image = std::make_shared<DecodedImage>();
    if (is_bwtx(bytes)) { // cooked by bowl-cook
        u32 w = 0, h = 0;
        if (decode_bwtx(bytes, w, h, image->cooked)) {
            image->w = static_cast<int>(w);
            image->h = static_cast<int>(h);
            image->pixels = image->cooked.data();
        } else {
            image->error = "a broken BWTX picture";
        }
        return image;
    }
    int channels = 0;
    if (bytes.size() > INT_MAX) { // stb counts bytes in an int
        image->error = "the file is too big";
        return image;
    }
    image->pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &image->w, &image->h, &channels, 4);
    if (image->pixels && (image->w <= 0 || image->h <= 0)) { // stb accepts a BMP 0 pixels high; a texture can't be
        stbi_image_free(image->pixels);
        image->pixels = nullptr;
        image->error = "the picture is empty";
    } else if (!image->pixels) {
        image->error = stbi_failure_reason();
    }
    return image;
}

std::unique_ptr<Texture> load_texture(AssetLoadContext& ctx, GpuDevice& device, Filter filter) {
    std::shared_ptr<DecodedImage> decoded;
    DecodedImage* image = ctx.prepared<DecodedImage>(); // decoded already by a background load
    if (!image) {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Texture", "file not found in any mount");
            return nullptr;
        }
        decoded = decode_image(*bytes);
        image = decoded.get();
    }
    if (!image->pixels) {
        log_asset_error(ctx.path(), "Texture", image->error ? image->error : "not a picture");
        return nullptr;
    }
    TextureDesc desc;
    desc.width = static_cast<u32>(image->w);
    desc.height = static_cast<u32>(image->h);
    desc.rgba8 = image->pixels;
    desc.filter = filter;
    desc.name = ctx.path();
    return device.create_texture(desc);
}

} // namespace

bool decode_png(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba) {
    auto image = decode_image(bytes);
    if (!image->pixels) return false;
    width = static_cast<u32>(image->w);
    height = static_cast<u32>(image->h);
    rgba.assign(image->pixels, image->pixels + static_cast<usize>(width) * height * 4);
    return true;
}

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
    assets.register_loader<ComputeShader>([&device](AssetLoadContext& ctx) { return load_compute_shader(ctx, device); });
    assets.register_loader<Texture>(
        [&device, default_filter](AssetLoadContext& ctx) { return load_texture(ctx, device, default_filter); });
    // Background loads decode the picture on the loading thread; only the upload is left for the main thread.
    assets.register_decoder<Texture>([](std::string_view, std::span<const u8> bytes) -> std::shared_ptr<void> {
        return decode_image(bytes);
    });
    assets.register_loader<Font>([&device](AssetLoadContext& ctx) -> std::unique_ptr<Font> {
        auto bytes = ctx.read(ctx.path());
        if (!bytes) {
            log_asset_error(ctx.path(), "Font", "file not found in any mount");
            return nullptr;
        }
        return Font::from_memory(&device, std::move(*bytes), ctx.path());
    });
    register_sprite_sheet_loader(assets);
    register_tilemap_loader(assets);
}

} // namespace bowl::gfx
