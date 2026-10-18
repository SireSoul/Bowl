// Cooked pictures (BWTX, what bowl-cook makes of PNG and JPEG files): a shipped game's files are data a stranger can
// replace. Every picture that decodes also encodes back to the same pixels.

#include "fuzz.hpp"

#include "bowl/gfx/bwtx.hpp"

using namespace bowl;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const u8> bytes{data, size};
    // Pictures up to 16384 a side decode (a gigabyte of pixels from 17 MB of runs), which takes the fuzzer seconds and
    // tells it nothing new: it skips those over 4096 x 4096.
    if (gfx::is_bwtx(bytes)) {
        auto u32_at = [&](usize i) { return u32(data[i]) | u32(data[i + 1]) << 8 | u32(data[i + 2]) << 16 | u32(data[i + 3]) << 24; };
        if (u64{u32_at(8)} * u32_at(12) > 4096 * 4096) return 0;
    }
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
    if (!gfx::decode_bwtx(bytes, width, height, rgba)) return 0;
    BOWL_FUZZ_CHECK(width > 0 && height > 0 && rgba.size() == usize{width} * height * 4);
    u32 w2 = 0, h2 = 0;
    std::vector<u8> again;
    BOWL_FUZZ_CHECK(gfx::decode_bwtx(gfx::encode_bwtx(width, height, rgba), w2, h2, again));
    BOWL_FUZZ_CHECK(w2 == width && h2 == height && again == rgba);
    return 0;
}
