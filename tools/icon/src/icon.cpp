#include "bowl/icon/icon.hpp"

#include <algorithm>
#include <cstring>
#include <format>

// Private copies of stb, so this library links next to the engine's own without clashing.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace bowl::icon {

std::optional<Image> decode_png(std::span<const u8> bytes) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels) return std::nullopt;
    Image image{static_cast<u32>(w), static_cast<u32>(h), std::vector<u8>(pixels, pixels + static_cast<usize>(w) * h * 4)};
    stbi_image_free(pixels);
    return image;
}

std::vector<u8> encode_png(const Image& image) {
    std::vector<u8> png;
    auto append = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    stbi_write_png_to_func(append, &png, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                           image.rgba.data(), static_cast<int>(image.width * 4));
    return png;
}

Image resize(const Image& image, u32 size, bool pixel_art) {
    // Square it first: the longer side fills the icon.
    u32 side = std::max(image.width, image.height);
    Image square{side, side, std::vector<u8>(static_cast<usize>(side) * side * 4, 0)};
    u32 ox = (side - image.width) / 2, oy = (side - image.height) / 2;
    for (u32 y = 0; y < image.height; ++y)
        std::memcpy(&square.rgba[((y + oy) * side + ox) * 4], &image.rgba[y * image.width * 4], image.width * 4);

    Image out{size, size, std::vector<u8>(static_cast<usize>(size) * size * 4)};
    if (pixel_art) {
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) {
                u32 sx = std::min(side - 1, static_cast<u32>((x + 0.5) * side / size));
                u32 sy = std::min(side - 1, static_cast<u32>((y + 0.5) * side / size));
                std::memcpy(&out.rgba[(y * size + x) * 4], &square.rgba[(sy * side + sx) * 4], 4);
            }
        return out;
    }
    stbir_resize_uint8_srgb(square.rgba.data(), static_cast<int>(side), static_cast<int>(side), 0, out.rgba.data(),
                            static_cast<int>(size), static_cast<int>(size), 0, STBIR_RGBA);
    return out;
}

namespace {
void put_be32(std::vector<u8>& out, u32 v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<u8>(v >> shift));
}
void put_le16(std::vector<u8>& out, u16 v) { out.push_back(static_cast<u8>(v)), out.push_back(static_cast<u8>(v >> 8)); }
void put_le32(std::vector<u8>& out, u32 v) { put_le16(out, static_cast<u16>(v)), put_le16(out, static_cast<u16>(v >> 16)); }
} // namespace

std::vector<u8> make_icns(const Image& image, bool pixel_art) {
    // The PNG entry types, by pixel size ("@2x" ones are the same pixels for Retina screens).
    struct Entry {
        const char* type;
        u32 size;
    };
    constexpr Entry entries[] = {{"icp4", 16},  {"icp5", 32},  {"ic11", 32},  {"ic12", 64},
                                 {"ic07", 128}, {"ic08", 256}, {"ic13", 256}, {"ic09", 512}, {"ic14", 512},
                                 {"ic10", 1024}};
    std::vector<u8> body;
    std::vector<u8> png;
    u32 last = 0;
    for (const Entry& e : entries) {
        if (e.size != last) png = encode_png(resize(image, e.size, pixel_art)), last = e.size;
        body.insert(body.end(), e.type, e.type + 4);
        put_be32(body, static_cast<u32>(png.size() + 8));
        body.insert(body.end(), png.begin(), png.end());
    }
    std::vector<u8> out{'i', 'c', 'n', 's'};
    put_be32(out, static_cast<u32>(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<u8> make_ico(const Image& image, bool pixel_art) {
    constexpr u32 sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<u8>> pngs;
    for (u32 s : sizes) pngs.push_back(encode_png(resize(image, s, pixel_art)));
    std::vector<u8> out;
    put_le16(out, 0);
    put_le16(out, 1); // icon
    put_le16(out, static_cast<u16>(pngs.size()));
    u32 offset = 6 + 16 * static_cast<u32>(pngs.size());
    for (usize i = 0; i < pngs.size(); ++i) {
        out.push_back(static_cast<u8>(sizes[i] == 256 ? 0 : sizes[i])); // 0 means 256
        out.push_back(static_cast<u8>(sizes[i] == 256 ? 0 : sizes[i]));
        out.push_back(0); // no palette
        out.push_back(0);
        put_le16(out, 1);  // planes
        put_le16(out, 32); // bits per pixel
        put_le32(out, static_cast<u32>(pngs[i].size()));
        put_le32(out, offset);
        offset += static_cast<u32>(pngs[i].size());
    }
    for (const auto& png : pngs) out.insert(out.end(), png.begin(), png.end());
    return out;
}

std::string make_cpp(std::span<const u8> png) {
    std::string out = "// Made by bowl-icon from the game's ICON. Don't edit.\n#include \"bowl/app/icon.hpp\"\n\nnamespace {\n"
                      "const unsigned char png[] = {";
    for (usize i = 0; i < png.size(); ++i) {
        if (i % 24 == 0) out += "\n   ";
        out += std::format(" {},", png[i]);
    }
    out += "\n};\nconst bowl::EmbeddedIcon icon(png, sizeof png);\n} // namespace\n";
    return out;
}

} // namespace bowl::icon
