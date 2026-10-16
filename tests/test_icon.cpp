#include "bowl/app/icon.hpp"
#include "bowl/icon/icon.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace bowl;

namespace {

// A 4 x 2 picture: red on the left half, blue on the right.
icon::Image two_colors() {
    icon::Image image{4, 2, std::vector<u8>(4 * 2 * 4)};
    for (u32 y = 0; y < 2; ++y)
        for (u32 x = 0; x < 4; ++x) {
            u8* p = &image.rgba[(y * 4 + x) * 4];
            p[0] = x < 2 ? 255 : 0, p[1] = 0, p[2] = x < 2 ? 0 : 255, p[3] = 255;
        }
    return image;
}

u32 be32(const std::vector<u8>& b, usize at) {
    return (u32{b[at]} << 24) | (u32{b[at + 1]} << 16) | (u32{b[at + 2]} << 8) | b[at + 3];
}
u32 le16(const std::vector<u8>& b, usize at) { return b[at] | (u32{b[at + 1]} << 8); }
u32 le32(const std::vector<u8>& b, usize at) { return le16(b, at) | (le16(b, at + 2) << 16); }

} // namespace

TEST_CASE("icon: resize squares the picture, centered, and keeps pixel art crisp") {
    icon::Image out = icon::resize(two_colors(), 8, true);
    CHECK(out.width == 8);
    CHECK(out.height == 8);
    auto at = [&](u32 x, u32 y) { return &out.rgba[(y * 8 + x) * 4]; };
    CHECK(at(0, 0)[3] == 0);   // above the picture: clear
    CHECK(at(0, 7)[3] == 0);   // below it
    CHECK(at(1, 3)[0] == 255); // left half red, hard edged
    CHECK(at(3, 4)[0] == 255);
    CHECK(at(4, 4)[2] == 255); // right half blue
    CHECK(at(4, 4)[0] == 0);

    icon::Image smooth = icon::resize(two_colors(), 64);
    CHECK(smooth.rgba.size() == 64 * 64 * 4);
    CHECK(smooth.rgba[(32 * 64 + 4) * 4 + 0] > 200); // still red well inside the left half
}

TEST_CASE("icon: PNG round trip") {
    icon::Image image = two_colors();
    auto png = icon::encode_png(image);
    REQUIRE(png.size() > 8);
    CHECK(png[1] == 'P');
    auto back = icon::decode_png(png);
    REQUIRE(back);
    CHECK(back->width == 4);
    CHECK(back->rgba == image.rgba);
    CHECK_FALSE(icon::decode_png(std::vector<u8>{1, 2, 3}));
}

TEST_CASE("icon: .icns holds every macOS size as PNG") {
    auto icns = icon::make_icns(two_colors(), true);
    REQUIRE(icns.size() > 8);
    CHECK(std::string(icns.begin(), icns.begin() + 4) == "icns");
    CHECK(be32(icns, 4) == icns.size());
    std::vector<std::pair<std::string, u32>> entries; // type, pixel size
    for (usize at = 8; at + 8 <= icns.size();) {
        u32 length = be32(icns, at + 4); // after the 4-letter type
        REQUIRE(at + length <= icns.size());
        REQUIRE(length > 8);
        std::vector<u8> png(icns.begin() + static_cast<std::ptrdiff_t>(at + 8), icns.begin() + static_cast<std::ptrdiff_t>(at + length));
        auto image = icon::decode_png(png);
        REQUIRE(image);
        entries.emplace_back(std::string(icns.begin() + static_cast<std::ptrdiff_t>(at), icns.begin() + static_cast<std::ptrdiff_t>(at + 4)),
                             image->width);
        at += length;
    }
    REQUIRE(entries.size() == 10);
    CHECK(entries.front() == std::pair<std::string, u32>{"icp4", 16});
    CHECK(entries.back() == std::pair<std::string, u32>{"ic10", 1024});
}

TEST_CASE("icon: .ico holds 16 to 256 as PNG") {
    auto ico = icon::make_ico(two_colors());
    CHECK(le16(ico, 0) == 0);
    CHECK(le16(ico, 2) == 1);
    u32 count = le16(ico, 4);
    REQUIRE(count == 7);
    for (u32 i = 0; i < count; ++i) {
        usize entry = 6 + i * 16;
        u32 size = ico[entry] == 0 ? 256 : ico[entry];
        u32 bytes = le32(ico, entry + 8), offset = le32(ico, entry + 12);
        REQUIRE(offset + bytes <= ico.size());
        auto image = icon::decode_png(std::span(ico).subspan(offset, bytes));
        REQUIRE(image);
        CHECK(image->width == size);
        CHECK(le16(ico, entry + 6) == 32);
    }
}

TEST_CASE("icon: the embedded icon source registers its PNG") {
    std::vector<u8> png = {137, 80, 78, 71};
    std::string cpp = icon::make_cpp(png);
    CHECK(cpp.find("#include \"bowl/app/icon.hpp\"") != std::string::npos);
    CHECK(cpp.find(" 137, 80, 78, 71,") != std::string::npos);
    CHECK(cpp.find("bowl::EmbeddedIcon icon(png, sizeof png)") != std::string::npos);

    // What that file does when the program starts.
    CHECK(embedded_icon().empty()); // the tests have no ICON
    static const u8 bytes[] = {1, 2, 3};
    EmbeddedIcon registered(bytes, sizeof bytes);
    CHECK(embedded_icon().size() == 3);
    CHECK(embedded_icon().data() == bytes);
    EmbeddedIcon none(nullptr, 0);
    CHECK(embedded_icon().empty());
}
