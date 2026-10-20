# bowl_shadergraph: Bowl Editor's shader graph (nodes, links, .shadergraph files, the GLSL a graph makes), without any
# windows, so the editor's Materials workspace drives it and the tests check it.

add_library(bowl_shadergraph STATIC src/shadergraph.cpp)
target_include_directories(bowl_shadergraph PUBLIC include)
target_link_libraries(bowl_shadergraph PUBLIC bowl_core)
bowl_configure_target(bowl_shadergraph)
set_target_properties(bowl_shadergraph PROPERTIES FOLDER tools)
