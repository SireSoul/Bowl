#include "bowl/gfx/bwtx.hpp"

#include <array>
#include <cstring>

// The pixel stream follows the QOI format ("Quite OK Image", qoiformat.org), written here from its published
// description. Each pixel is one of: a run of the previous pixel, a slot in a table of 64 recently seen pixels (by
// hash), a small difference from the previous pixel, or the colour in full.

namespace bowl::gfx {

namespace {

constexpr usize header_size = 16;
constexpr u8 end_marker[8] = {0, 0, 0, 0, 0, 0, 0, 1};

constexpr u8 op_index = 0x00; // 00iiiiii: table slot i
constexpr u8 op_diff = 0x40;  // 01rrggbb: r, g, b each changed by -2..1 (stored + 2)
constexpr u8 op_luma = 0x80;  // 10gggggg, rrrrbbbb: g changed by -32..31 (+ 32); r - g and b - g by -8..7 (+ 8)
constexpr u8 op_run = 0xC0;   // 11nnnnnn: the previous pixel n + 1 more times (1..62)
constexpr u8 op_rgb = 0xFE;   // then r, g, b; alpha as before
constexpr u8 op_rgba = 0xFF;  // then r, g, b, a

struct Pixel {
    u8 r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Pixel&) const = default;
};

usize slot(Pixel p) { return (p.r * 3u + p.g * 5u + p.b * 7u + p.a * 11u) % 64u; }

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 get_u32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

} // namespace

bool is_bwtx(std::span<const u8> bytes) {
    return bytes.size() >= header_size && std::memcmp(bytes.data(), "BWTX", 4) == 0;
}

std::vector<u8> encode_bwtx(u32 width, u32 height, std::span<const u8> rgba) {
    usize count = usize{width} * height;
    std::vector<u8> out;
    out.reserve(header_size + count * 2 + sizeof end_marker);
    out.insert(out.end(), {'B', 'W', 'T', 'X', 1, 4, 0, 0});
    put_u32(out, width);
    put_u32(out, height);

    std::array<Pixel, 64> seen{};
    Pixel prev{0, 0, 0, 255};
    u32 run = 0;
    for (usize i = 0; i < count; ++i) {
        const u8* s = rgba.data() + i * 4;
        Pixel px{s[0], s[1], s[2], s[3]};
        if (px == prev) {
            if (++run == 62 || i + 1 == count) {
                out.push_back(static_cast<u8>(op_run | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run) {
            out.push_back(static_cast<u8>(op_run | (run - 1)));
            run = 0;
        }
        usize h = slot(px);
        if (seen[h] == px) {
            out.push_back(static_cast<u8>(op_index | h));
        } else {
            seen[h] = px;
            if (px.a == prev.a) {
                // Differences wrap around, as bytes do: 255 to 0 is +1.
                auto dr = static_cast<i8>(px.r - prev.r), dg = static_cast<i8>(px.g - prev.g),
                     db = static_cast<i8>(px.b - prev.b);
                int dr_dg = dr - dg, db_dg = db - dg;
                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                    out.push_back(static_cast<u8>(op_diff | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2)));
                } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                    out.push_back(static_cast<u8>(op_luma | (dg + 32)));
                    out.push_back(static_cast<u8>((dr_dg + 8) << 4 | (db_dg + 8)));
                } else {
                    out.insert(out.end(), {op_rgb, px.r, px.g, px.b});
                }
            } else {
                out.insert(out.end(), {op_rgba, px.r, px.g, px.b, px.a});
            }
        }
        prev = px;
    }
    out.insert(out.end(), std::begin(end_marker), std::end(end_marker));
    return out;
}

bool decode_bwtx(std::span<const u8> bytes, u32& width, u32& height, std::vector<u8>& rgba) {
    if (!is_bwtx(bytes) || bytes[4] != 1 || bytes[5] != 4) return false;
    u32 w = get_u32(bytes.data() + 8), h = get_u32(bytes.data() + 12);
    if (w == 0 || h == 0 || w > bwtx_max_side || h > bwtx_max_side) return false;
    usize count = usize{w} * h;
    // One byte is at most 62 pixels: a picture bigger than its stream could hold isn't one, and gets no memory.
    std::span<const u8> in = bytes.subspan(header_size);
    if (count > in.size() * 62) return false;

    std::vector<u8> out(count * 4);
    std::array<Pixel, 64> seen{};
    Pixel px{0, 0, 0, 255};
    usize pos = 0;
    for (usize i = 0; i < count;) {
        if (pos >= in.size()) return false;
        u8 op = in[pos++];
        u32 repeat = 1;
        if (op == op_rgb || op == op_rgba) {
            usize n = op == op_rgb ? 3 : 4;
            if (in.size() - pos < n) return false;
            px.r = in[pos];
            px.g = in[pos + 1];
            px.b = in[pos + 2];
            if (op == op_rgba) px.a = in[pos + 3];
            pos += n;
        } else if ((op & 0xC0) == op_index) {
            px = seen[op & 0x3F];
        } else if ((op & 0xC0) == op_diff) {
            px.r = static_cast<u8>(px.r + ((op >> 4) & 3) - 2);
            px.g = static_cast<u8>(px.g + ((op >> 2) & 3) - 2);
            px.b = static_cast<u8>(px.b + (op & 3) - 2);
        } else if ((op & 0xC0) == op_luma) {
            if (pos >= in.size()) return false;
            u8 next = in[pos++];
            int dg = (op & 0x3F) - 32;
            px.r = static_cast<u8>(px.r + dg + (next >> 4) - 8);
            px.g = static_cast<u8>(px.g + dg);
            px.b = static_cast<u8>(px.b + dg + (next & 0x0F) - 8);
        } else {
            repeat = (op & 0x3F) + 1u;
            if (repeat > count - i) return false;
        }
        seen[slot(px)] = px;
        for (u32 k = 0; k < repeat; ++k, ++i) std::memcpy(out.data() + i * 4, &px, 4);
    }
    width = w;
    height = h;
    rgba = std::move(out);
    return true;
}

} // namespace bowl::gfx
