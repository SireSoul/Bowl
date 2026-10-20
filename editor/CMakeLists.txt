# Bowl Editor: a separate program, built on the engine, for laying out levels (.world files), editing the game's data
# files and painting its art. It needs the dev tools (Dear ImGui), so it's only built when BOWL_DEV_TOOLS is on.

add_executable(bowl-editor
    src/main.cpp
    src/code_editor.cpp
    src/content.cpp
    src/content_maps.cpp
    src/content_tools.cpp
    src/content_world.cpp
    src/document.cpp
    src/editor.cpp
    src/data.cpp
    src/game.cpp
    src/materials.cpp
    src/materials_shaders.cpp
    src/paint.cpp
    src/paint_studio.cpp
    src/panels.cpp
    src/regions.cpp
    src/ui.cpp)
bowl_add_game(bowl-editor)
target_link_libraries(bowl-editor PRIVATE bowl_stb bowl_paint bowl_shadergraph) # stb_image, to read pictures for painting
# The Materials workspace compiles shaders as they're typed.
if(TARGET bowl_shaderc)
    target_link_libraries(bowl-editor PRIVATE bowl_shaderc)
    target_compile_definitions(bowl-editor PRIVATE BOWL_EDITOR_SHADERC=1)
endif()
set_target_properties(bowl-editor PROPERTIES FOLDER tools)
# The editor loads games' code libraries (--game) to learn their component types. Those libraries call into the
# engine, so the editor exports it.
bowl_export_engine(bowl-editor)

# On Windows a game's code library imports the engine from its own executable, so the editor can't load it. Each
# hot-reloadable game gets a second copy here, <game>_editor_code, linked against the editor instead.
if(WIN32)
    get_property(games GLOBAL PROPERTY BOWL_EDITOR_GAMES)
    foreach(game IN LISTS games)
        get_target_property(sources ${game}_code BOWL_GAME_SOURCES)
        add_library(${game}_editor_code MODULE ${sources})
        target_compile_definitions(${game}_editor_code PRIVATE BOWL_HOT_RELOAD_LIBRARY=1)
        target_link_libraries(${game}_editor_code PRIVATE $<COMPILE_ONLY:bowl::engine> bowl-editor)
        bowl_configure_target(${game}_editor_code)
        set_target_properties(${game}_editor_code PROPERTIES FOLDER tools)
    endforeach()
endif()
