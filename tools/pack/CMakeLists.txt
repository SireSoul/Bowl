# bowl-pack: puts asset folders into archives (.bowlpak) for shipping builds; BOWL_PACK_ASSETS runs it

add_executable(bowl-pack src/main.cpp)
target_link_libraries(bowl-pack PRIVATE bowl_core)
bowl_configure_target(bowl-pack)
set_target_properties(bowl-pack PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
