# bowl_paint: Bowl Editor's pixel painting (layers, selections, tools, adjustments, undo), without any windows, so the
# editor's Paint workspace drives it and the tests check it.

add_library(bowl_paint STATIC src/paint.cpp)
target_include_directories(bowl_paint PUBLIC include)
target_link_libraries(bowl_paint PUBLIC bowl_core)
bowl_configure_target(bowl_paint)
set_target_properties(bowl_paint PROPERTIES FOLDER tools)
