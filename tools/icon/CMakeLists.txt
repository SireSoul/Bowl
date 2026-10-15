# ph_icon: application icons from one picture (.icns, .ico, PNGs, the embedded window icon), in memory so it's testable
# ph-icon: the command-line tool around it; ph_add_game's ICON runs it

add_library(ph_icon STATIC src/icon.cpp)
add_library(ph::icon ALIAS ph_icon)
target_include_directories(ph_icon PUBLIC include)
target_link_libraries(ph_icon PUBLIC ph_core PRIVATE ph_stb)
ph_configure_target(ph_icon)
set_target_properties(ph_icon PROPERTIES FOLDER tools)

add_executable(ph-icon src/main.cpp)
target_link_libraries(ph-icon PRIVATE ph_icon)
ph_configure_target(ph-icon)
set_target_properties(ph-icon PROPERTIES FOLDER tools)
