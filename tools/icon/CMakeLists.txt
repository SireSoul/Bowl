# bowl_icon: application icons from one picture (.icns, .ico, PNGs, the embedded window icon), in memory so it's
# testable
# bowl-icon: the command-line tool around it; bowl_add_game's ICON runs it

add_library(bowl_icon STATIC src/icon.cpp)
add_library(bowl::icon ALIAS bowl_icon)
target_include_directories(bowl_icon PUBLIC include)
target_link_libraries(bowl_icon PUBLIC bowl_core PRIVATE bowl_stb)
bowl_configure_target(bowl_icon)
set_target_properties(bowl_icon PROPERTIES FOLDER tools)

add_executable(bowl-icon src/main.cpp)
target_link_libraries(bowl-icon PRIVATE bowl_icon)
bowl_configure_target(bowl-icon)
set_target_properties(bowl-icon PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
