#pragma once

// Tiny fonts made in memory, for the font tests and the font fuzzer's seeds (tests/fuzz).

#include "bowl/core/types.hpp"
#include "bowl/icon/icon.hpp"

#include <string>
#include <utility>
#include <vector>

namespace bowl::testing {

// A tiny TrueType font made in memory: 'A' is glyph 1 (a square), U+1F600 is glyph 2 (a color glyph, if a color
// table is given), glyph 3 is a small square COLR layers use.
struct TinyFont {
    std::vector<std::pair<std::string, std::vector<u8>>> tables;

    static void be16(std::vector<u8>& out, u32 v) {
        out.push_back(static_cast<u8>(v >> 8)), out.push_back(static_cast<u8>(v));
    }
    static void be32(std::vector<u8>& out, u32 v) { be16(out, v >> 16), be16(out, v & 0xFFFF); }

    static std::vector<u8> square(i16 x0, i16 y0, i16 x1, i16 y1) {
        std::vector<u8> g;
        be16(g, 1); // one contour
        be16(g, static_cast<u16>(x0)), be16(g, static_cast<u16>(y0)), be16(g, static_cast<u16>(x1)),
            be16(g, static_cast<u16>(y1));
        be16(g, 3);                                 // last point
        be16(g, 0);                                 // no instructions
        for (int i = 0; i < 4; ++i) g.push_back(1); // on-curve, 16-bit deltas
        for (i32 dx : {i32{x0}, x1 - x0, 0, x0 - x1}) be16(g, static_cast<u16>(dx));
        for (i32 dy : {i32{y0}, 0, y1 - y0, 0}) be16(g, static_cast<u16>(dy));
        return g;
    }

    TinyFont() {
        std::vector<u8> head(54, 0);
        head[1] = 1;                                                        // version 1.0
        head[12] = 0x5F, head[13] = 0x0F, head[14] = 0x3C, head[15] = 0xF5; // magic
        head[18] = 1000 >> 8, head[19] = 1000 & 0xFF;                       // units per em
        tables.push_back({"head", head});
        std::vector<u8> hhea(36, 0);
        hhea[1] = 1;
        hhea[4] = 800 >> 8, hhea[5] = 800 & 0xFF; // ascent
        u16 descent = static_cast<u16>(-200);
        hhea[6] = static_cast<u8>(descent >> 8), hhea[7] = static_cast<u8>(descent);
        hhea[35] = 4; // horizontal metrics for 4 glyphs
        tables.push_back({"hhea", hhea});
        std::vector<u8> maxp;
        be32(maxp, 0x00005000), be16(maxp, 4);
        tables.push_back({"maxp", maxp});
        std::vector<u8> hmtx;
        for (int i = 0; i < 4; ++i) be16(hmtx, 1000), be16(hmtx, 0);
        tables.push_back({"hmtx", hmtx});
        std::vector<u8> cmap;
        be16(cmap, 0), be16(cmap, 1), be16(cmap, 3), be16(cmap, 10), be32(cmap, 12);
        be16(cmap, 12), be16(cmap, 0), be32(cmap, 16 + 2 * 12), be32(cmap, 0), be32(cmap, 2);
        be32(cmap, 'A'), be32(cmap, 'A'), be32(cmap, 1);
        be32(cmap, 0x1F600), be32(cmap, 0x1F600), be32(cmap, 2);
        tables.push_back({"cmap", cmap});
        std::vector<std::vector<u8>> glyphs = {
            {}, square(100, 0, 900, 800), square(100, 0, 900, 800), square(400, 300, 600, 500)};
        std::vector<u8> glyf, loca;
        for (auto& g : glyphs) {
            if (g.size() % 2) g.push_back(0);
            be16(loca, static_cast<u32>(glyf.size() / 2));
            glyf.insert(glyf.end(), g.begin(), g.end());
        }
        be16(loca, static_cast<u32>(glyf.size() / 2));
        tables.push_back({"loca", loca});
        tables.push_back({"glyf", glyf});
    }

    std::vector<u8> build() const {
        std::vector<u8> out;
        be32(out, 0x00010000), be16(out, static_cast<u32>(tables.size())), be16(out, 0), be16(out, 0), be16(out, 0);
        u32 offset = 12 + 16 * static_cast<u32>(tables.size());
        std::vector<u8> data;
        for (const auto& [tag, bytes] : tables) {
            out.insert(out.end(), tag.begin(), tag.end());
            be32(out, 0), be32(out, offset + static_cast<u32>(data.size())), be32(out, static_cast<u32>(bytes.size()));
            data.insert(data.end(), bytes.begin(), bytes.end());
            while (data.size() % 4) data.push_back(0);
        }
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }
};

// A size x size PNG of one color.
inline std::vector<u8> png_of(u32 size, u8 r, u8 g, u8 b) {
    icon::Image image{size, size, {}};
    for (u32 i = 0; i < size * size; ++i) image.rgba.insert(image.rgba.end(), {r, g, b, 255});
    return icon::encode_png(image);
}

// Color layers for the tiny font's 😀 (glyph 2) in COLR/CPAL: the big square in red, the small one over it in blue.
inline void add_colr(TinyFont& tiny) {
    std::vector<u8> colr, cpal;
    TinyFont::be16(colr, 0), TinyFont::be16(colr, 1), TinyFont::be32(colr, 14), TinyFont::be32(colr, 20),
        TinyFont::be16(colr, 2);
    TinyFont::be16(colr, 2), TinyFont::be16(colr, 0), TinyFont::be16(colr, 2); // glyph 2: layers 0 and 1
    TinyFont::be16(colr, 1), TinyFont::be16(colr, 0);                          // the big square, red
    TinyFont::be16(colr, 3), TinyFont::be16(colr, 1);                          // the small one, blue
    TinyFont::be16(cpal, 0), TinyFont::be16(cpal, 2), TinyFont::be16(cpal, 1), TinyFont::be16(cpal, 2);
    TinyFont::be32(cpal, 14), TinyFont::be16(cpal, 0);
    cpal.insert(cpal.end(), {0, 0, 255, 255, 255, 0, 0, 255}); // BGRA: red, blue
    tiny.tables.push_back({"COLR", colr});
    tiny.tables.push_back({"CPAL", cpal});
}

// Gives the tiny font's 😀 (glyph 2) a 20 x 20 CBDT picture of one color, drawn 16 px above the baseline.
inline void add_cbdt(TinyFont& tiny, const std::vector<u8>& png) {
    std::vector<u8> cblc, cbdt;
    TinyFont::be16(cblc, 3), TinyFont::be16(cblc, 0), TinyFont::be32(cblc, 1);
    TinyFont::be32(cblc, 56), TinyFont::be32(cblc, 16), TinyFont::be32(cblc, 1), TinyFont::be32(cblc, 0);
    cblc.insert(cblc.end(), 24, 0); // line metrics
    TinyFont::be16(cblc, 2), TinyFont::be16(cblc, 2);
    cblc.insert(cblc.end(), {20, 20, 32, 1});                                   // ppem x, y, bit depth, flags
    TinyFont::be16(cblc, 2), TinyFont::be16(cblc, 2), TinyFont::be32(cblc, 8);  // glyphs 2..2, subtable after this
    TinyFont::be16(cblc, 1), TinyFont::be16(cblc, 17), TinyFont::be32(cblc, 0); // index format 1, image format 17
    TinyFont::be32(cblc, 4), TinyFont::be32(cblc, 4 + 5 + 4 + static_cast<u32>(png.size()));
    TinyFont::be16(cbdt, 3), TinyFont::be16(cbdt, 0);
    cbdt.insert(cbdt.end(), {20, 20, 0, 16, 20}); // height, width, bearing x, bearing y, advance
    TinyFont::be32(cbdt, static_cast<u32>(png.size()));
    cbdt.insert(cbdt.end(), png.begin(), png.end());
    tiny.tables.push_back({"CBLC", cblc});
    tiny.tables.push_back({"CBDT", cbdt});
}

// Gives the tiny font's 😀 (glyph 2) a 20 ppem sbix picture, its bottom 4 px below the baseline.
inline void add_sbix(TinyFont& tiny, const std::vector<u8>& png) {
    std::vector<u8> sbix;
    TinyFont::be16(sbix, 1), TinyFont::be16(sbix, 1), TinyFont::be32(sbix, 1), TinyFont::be32(sbix, 12);
    TinyFont::be16(sbix, 20), TinyFont::be16(sbix, 72); // the strike: 20 ppem
    u32 data = 4 + 4 * 5;                               // after the 5 glyph offsets
    u32 glyph2 = 8 + static_cast<u32>(png.size());
    for (u32 offset : {data, data, data, data + glyph2, data + glyph2}) TinyFont::be32(sbix, offset);
    TinyFont::be16(sbix, 0), TinyFont::be16(sbix, static_cast<u16>(-4)); // origin: the picture's bottom 4 px below
    sbix.insert(sbix.end(), {'p', 'n', 'g', ' '});
    sbix.insert(sbix.end(), png.begin(), png.end());
    tiny.tables.push_back({"sbix", sbix});
}

} // namespace bowl::testing
