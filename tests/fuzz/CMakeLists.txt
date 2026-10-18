# Fuzzers (BOWL_FUZZ): one libFuzzer program per parser that reads data a stranger could control, from network packets
# to fonts and save files. CONTRIBUTING.md says how to build and run them; run.sh runs them all for a while.
#
# Each fuzz_<name> starts from seeds bowl_fuzz_seeds writes into <build>/fuzz/seeds/<name>, made from what the tests use.

set(BOWL_FUZZ_DIR "${CMAKE_BINARY_DIR}/fuzz")

add_executable(bowl_fuzz_seeds seeds.cpp)
target_link_libraries(bowl_fuzz_seeds PRIVATE bowl_core bowl_gfx bowl_net bowl_icon bowl_stb)
target_compile_definitions(bowl_fuzz_seeds PRIVATE BOWL_ENGINE_ASSET_SOURCE_DIR="${BOWL_ENGINE_ASSET_SOURCE_DIR}"
    BOWL_SAMPLES_DIR="${PROJECT_SOURCE_DIR}/samples")
bowl_configure_target(bowl_fuzz_seeds)
set_target_properties(bowl_fuzz_seeds PROPERTIES FOLDER tests/fuzz)

add_custom_command(OUTPUT "${BOWL_FUZZ_DIR}/seeds/.stamp"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${BOWL_FUZZ_DIR}/seeds"
    COMMAND bowl_fuzz_seeds "${BOWL_FUZZ_DIR}/seeds"
    COMMAND "${CMAKE_COMMAND}" -E touch "${BOWL_FUZZ_DIR}/seeds/.stamp"
    DEPENDS bowl_fuzz_seeds
    COMMENT "Writing fuzzer seeds")
add_custom_target(bowl_fuzz ALL DEPENDS "${BOWL_FUZZ_DIR}/seeds/.stamp")
set_target_properties(bowl_fuzz PROPERTIES FOLDER tests/fuzz)

# bowl_fuzzer(<name> <libraries...>): fuzz_<name> from fuzz_<name>.cpp. The `bowl_fuzz` target builds every fuzzer
# and its seeds; ctest runs each over its seeds once, so a fuzz build also checks they still pass.
function(bowl_fuzzer name)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE ${ARGN})
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    bowl_configure_target(fuzz_${name})
    set_target_properties(fuzz_${name} PROPERTIES FOLDER tests/fuzz RUNTIME_OUTPUT_DIRECTORY "${BOWL_FUZZ_DIR}")
    add_dependencies(bowl_fuzz fuzz_${name})
    if(BOWL_BUILD_TESTS)
        add_test(NAME fuzz_${name} COMMAND fuzz_${name} -runs=0 "${BOWL_FUZZ_DIR}/seeds/${name}")
    endif()
endfunction()

bowl_fuzzer(json bowl_core)
bowl_fuzzer(xml bowl_core)
bowl_fuzzer(archive bowl_core)
bowl_fuzzer(world bowl_ecs)
bowl_fuzzer(tilemap bowl_gfx)
bowl_fuzzer(sprite_sheet bowl_gfx)
bowl_fuzzer(sound_event bowl_audio)
bowl_fuzzer(png bowl_gfx bowl_stb)
bowl_fuzzer(bwtx bowl_gfx)
bowl_fuzzer(font bowl_gfx)
target_include_directories(fuzz_font PRIVATE "${BOWL_ENGINE_SOURCE_DIR}/gfx/src") # color_glyphs.hpp
bowl_fuzzer(address bowl_net)
bowl_fuzzer(host bowl_net)
bowl_fuzzer(rendezvous bowl_net)
