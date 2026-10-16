# ph-pack: puts asset folders into archives (.phpak) for shipping builds; PH_PACK_ASSETS runs it

add_executable(ph-pack src/main.cpp)
target_link_libraries(ph-pack PRIVATE ph_core)
ph_configure_target(ph-pack)
set_target_properties(ph-pack PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${PH_TOOLS_DIR}")
