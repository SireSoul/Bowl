#pragma once

// Shared by the fuzzers and the seed writer: inputs that are a list of steps rather than one file (the network
// fuzzers), read and written the same way.

#include "bowl/core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace bowl::fuzz {

// Takes values off the front of the input. Past the end it gives zeros and empty spans, so every input means
// something.
struct Input {
    std::span<const u8> bytes;

    bool empty() const { return bytes.empty(); }
    u8 byte() {
        if (bytes.empty()) return 0;
        u8 b = bytes[0];
        bytes = bytes.subspan(1);
        return b;
    }
    u16 u16_() {
        u16 lo = byte();
        return static_cast<u16>(lo | (byte() << 8));
    }
    std::span<const u8> take(usize n) {
        n = std::min(n, bytes.size());
        auto s = bytes.first(n);
        bytes = bytes.subspan(n);
        return s;
    }
    // A u16 length, then that many bytes.
    std::span<const u8> chunk() { return take(u16_()); }
};

struct Output {
    std::vector<u8> bytes;

    void byte(u8 b) { bytes.push_back(b); }
    void u16_(u16 v) { byte(static_cast<u8>(v)), byte(static_cast<u8>(v >> 8)); }
    void chunk(std::span<const u8> data) {
        u16_(static_cast<u16>(data.size()));
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
};

// Inputs with more than one file in them (a map and the tileset it refers to) are split at NUL bytes.
inline std::vector<std::string_view> split_files(std::span<const u8> data) {
    std::string_view all(reinterpret_cast<const char*>(data.data()), data.size());
    std::vector<std::string_view> files;
    for (;;) {
        usize nul = all.find('\0');
        files.push_back(all.substr(0, nul));
        if (nul == std::string_view::npos) return files;
        all.remove_prefix(nul + 1);
    }
}

} // namespace bowl::fuzz

// A broken promise the fuzzer should report (like a sanitizer does), even in builds without asserts.
#define BOWL_FUZZ_CHECK(condition)                                                                                     \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)
