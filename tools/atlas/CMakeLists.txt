# bowl_atlas: packs pictures into an atlas (pixels in memory, so it's testable)
# bowl-atlas: the command-line tool around it (reads PNG files, writes the atlas PNG and sprite-sheet JSON)

add_library(bowl_atlas STATIC src/atlas.cpp)
add_library(bowl::atlas ALIAS bowl_atlas)
target_include_directories(bowl_atlas PUBLIC include)
target_link_libraries(bowl_atlas PUBLIC bowl_core)
bowl_configure_target(bowl_atlas)
set_target_properties(bowl_atlas PROPERTIES FOLDER tools)

add_executable(bowl-atlas src/main.cpp)
target_link_libraries(bowl-atlas PRIVATE bowl_atlas bowl_stb)
bowl_configure_target(bowl-atlas)
set_target_properties(bowl-atlas PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
