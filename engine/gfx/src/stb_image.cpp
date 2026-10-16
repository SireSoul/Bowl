// stb_image and stb_image_write, for textures, window icons, screenshots and Tiled's zlib layers.
//
// This file is built with -fwrapv (see engine/CMakeLists.txt): stb_image's JPEG decoder overflows int on corrupt data,
// expecting it to wrap as its SIMD version does, and -fwrapv makes that wrap defined instead of undefined.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
// No GPU Bowl runs on takes a bigger texture; without a limit, a file of a few bytes whose header claims a huge picture
// makes stb allocate gigabytes.
#define STBI_MAX_DIMENSIONS 16384
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
