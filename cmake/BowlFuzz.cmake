# Fuzzing (BOWL_FUZZ): the programs in tests/fuzz feed the engine's parsers inputs libFuzzer makes up, and
# AddressSanitizer and UBSan stop at the first out-of-bounds access, leak or undefined behavior. Everything added after
# this file (the engine, tools, tests and fuzzers) is built with the sanitizers and libFuzzer's coverage counters;
# third-party libraries added before it aren't, except header-only ones compiled into engine files (stb), which are.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
    message(FATAL_ERROR "BOWL_FUZZ needs Clang (libFuzzer comes with it); this is ${CMAKE_CXX_COMPILER_ID}")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
check_cxx_source_compiles([[
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
]] BOWL_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT BOWL_HAVE_LIBFUZZER)
    message(FATAL_ERROR "BOWL_FUZZ: ${CMAKE_CXX_COMPILER} can't build libFuzzer programs. Apple's Clang has no "
                        "libFuzzer; use LLVM's (brew install llvm, then configure with "
                        "-DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang "
                        "-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++).")
endif()

# Every UBSan finding stops the program, so the fuzzer sees it as a crash and keeps the input.
add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
                    -fsanitize=fuzzer-no-link)
add_link_options(-fsanitize=address,undefined)
