add_executable(ph_tests
    main.cpp
    test_assets.cpp
    test_audio.cpp
    test_ecs.cpp
    test_input.cpp
    test_jobs.cpp
    test_json.cpp
    test_math.cpp
    test_schedule.cpp
    test_shaderc.cpp
    test_sprite_sheet.cpp
    test_text.cpp)
target_link_libraries(ph_tests PRIVATE ph_core ph_ecs ph_platform ph_gfx ph_audio doctest::doctest)
target_compile_definitions(ph_tests PRIVATE PH_ENGINE_ASSET_SOURCE_DIR="${PH_ENGINE_ASSET_SOURCE_DIR}"
    PH_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
if(TARGET ph_shaderc)
    target_link_libraries(ph_tests PRIVATE ph_shaderc)
    target_compile_definitions(ph_tests PRIVATE PH_TEST_SHADERC=1)
endif()
ph_configure_target(ph_tests)
# doctest's own std forward declarations break on recent MSVC STLs.
target_compile_definitions(ph_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
set_target_properties(ph_tests PROPERTIES FOLDER tests)
add_test(NAME ph_tests COMMAND ph_tests)

# Not a test: prints ECS throughput numbers. Run it from a Release build.
add_executable(ph_bench_ecs bench_ecs.cpp)
target_link_libraries(ph_bench_ecs PRIVATE ph_ecs)
ph_configure_target(ph_bench_ecs)
set_target_properties(ph_bench_ecs PROPERTIES FOLDER tests)
