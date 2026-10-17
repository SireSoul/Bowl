# bowl_cook: turns asset folders into what shipping builds load fastest (binary JSON, flattened maps, BWTX pictures)
# bowl-cook: the command-line tool around it; BOWL_PACK_ASSETS builds run it before bowl-pack (BOWL_COOK_ASSETS)

add_library(bowl_cook STATIC src/cook.cpp)
add_library(bowl::cook ALIAS bowl_cook)
target_include_directories(bowl_cook PUBLIC include)
target_link_libraries(bowl_cook PUBLIC bowl_core PRIVATE bowl_gfx)
bowl_configure_target(bowl_cook)
set_target_properties(bowl_cook PROPERTIES FOLDER tools)

add_executable(bowl-cook src/main.cpp)
target_link_libraries(bowl-cook PRIVATE bowl_cook)
bowl_configure_target(bowl-cook)
set_target_properties(bowl-cook PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
