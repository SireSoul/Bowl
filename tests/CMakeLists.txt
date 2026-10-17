add_executable(bowl_tests
    main.cpp
    test_assets.cpp
    test_audio.cpp
    test_bidi.cpp
    test_collision.cpp
    test_ecs.cpp
    test_apidoc.cpp
    test_hierarchy.cpp
    test_icon.cpp
    test_input.cpp
    test_jobs.cpp
    test_localization.cpp
    test_local_types.cpp
    local_types_other.cpp
    test_json.cpp
    test_math.cpp
    test_net.cpp
    test_dev_link.cpp
    test_particles.cpp
    test_reflect.cpp
    test_replication.cpp
    test_scene.cpp
    test_settings.cpp
    test_schedule.cpp
    test_shaderc.cpp
    test_sprite.cpp
    test_sprite_sheet.cpp
    test_text.cpp
    test_tilemap.cpp
    test_xml.cpp
    test_ui.cpp)
target_link_libraries(bowl_tests PRIVATE bowl::engine bowl_atlas bowl_icon bowl_apidoc doctest::doctest)
target_compile_definitions(bowl_tests PRIVATE BOWL_ENGINE_ASSET_SOURCE_DIR="${BOWL_ENGINE_ASSET_SOURCE_DIR}"
    BOWL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
if(BOWL_BOX2D)
    target_sources(bowl_tests PRIVATE test_rigid_bodies.cpp)
endif()
if(BOWL_OPUS)
    target_link_libraries(bowl_tests PRIVATE ogg opus)
    target_compile_definitions(bowl_tests PRIVATE BOWL_OPUS=1)
endif()
if(TARGET bowl_shaderc)
    target_link_libraries(bowl_tests PRIVATE bowl_shaderc)
    target_compile_definitions(bowl_tests PRIVATE BOWL_TEST_SHADERC=1)
endif()
bowl_configure_target(bowl_tests)
# doctest's own std forward declarations break on recent MSVC STLs.
target_compile_definitions(bowl_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
set_target_properties(bowl_tests PROPERTIES FOLDER tests)
add_test(NAME bowl_tests COMMAND bowl_tests)

# Not a test: prints ECS throughput numbers. Run it from a Release build.
add_executable(bowl_bench_ecs bench_ecs.cpp)
target_link_libraries(bowl_bench_ecs PRIVATE bowl_ecs)
bowl_configure_target(bowl_bench_ecs)
set_target_properties(bowl_bench_ecs PROPERTIES FOLDER tests)
