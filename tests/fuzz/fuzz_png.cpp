// Pictures: textures and window icons (gfx::decode_png reads every format the texture loader does).

#include "fuzz.hpp"

#include "bowl/gfx/renderer.hpp"

#include <stb_image.h> // declarations only: implemented inside bowl_gfx

using namespace bowl;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Pictures up to 16384 pixels a side decode (a gigabyte of pixels), which takes the fuzzer seconds and tells it
    // nothing new: it skips those over 4096 a side.
    int w = 0, h = 0, channels = 0;
    if (stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels) && i64{w} * h > 4096 * 4096) return 0;
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
    if (gfx::decode_png({data, size}, width, height, rgba))
        BOWL_FUZZ_CHECK(width > 0 && height > 0 && rgba.size() == usize{width} * height * 4);
    return 0;
}
