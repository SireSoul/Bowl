#pragma once

#include "bowl/core/types.hpp"

#include <string>
#include <string_view>

namespace bowl::utf8 {

inline constexpr u32 replacement = 0xFFFD;

// Decodes the code point at s[i] and advances i past it. Malformed bytes decode as U+FFFD (one byte at a time), so
// broken text renders visibly instead of hanging or crashing.
constexpr u32 next(std::string_view s, usize& i) {
    auto byte = [&](usize k) { return static_cast<u8>(s[k]); };
    u8 b0 = byte(i);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    usize len = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
        ++i;
        return replacement;
    }
    u32 cp = b0 & (0x7F >> len);
    for (usize k = 1; k < len; ++k) {
        u8 b = byte(i + k);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return replacement;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    // Reject overlong encodings and surrogates.
    constexpr u32 min_for_len[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return replacement;
    }
    i += len;
    return cp;
}

inline void append(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace bowl::utf8
