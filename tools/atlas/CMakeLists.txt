# ph_atlas: packs pictures into an atlas (pixels in memory, so it's testable)
# ph-atlas: the command-line tool around it (reads PNG files, writes the atlas PNG and sprite-sheet JSON)

add_library(ph_atlas STATIC src/atlas.cpp)
add_library(ph::atlas ALIAS ph_atlas)
target_include_directories(ph_atlas PUBLIC include)
target_link_libraries(ph_atlas PUBLIC ph_core)
ph_configure_target(ph_atlas)
set_target_properties(ph_atlas PROPERTIES FOLDER tools)

add_executable(ph-atlas src/main.cpp)
target_link_libraries(ph-atlas PRIVATE ph_atlas ph_stb)
ph_configure_target(ph-atlas)
set_target_properties(ph-atlas PROPERTIES FOLDER tools)
