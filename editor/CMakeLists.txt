# Bowl Editor: a separate program, built on the engine, for laying out levels (.world files), editing the game's data
# files and painting its art. It needs the dev tools (Dear ImGui), so it's only built when PH_DEV_TOOLS is on.

add_executable(bowl-editor
    src/main.cpp
    src/document.cpp
    src/editor.cpp
    src/data.cpp
    src/paint.cpp
    src/panels.cpp
    src/regions.cpp
    src/ui.cpp)
ph_add_game(bowl-editor)
target_link_libraries(bowl-editor PRIVATE ph_stb) # stb_image, to read pictures for painting
set_target_properties(bowl-editor PROPERTIES FOLDER tools)
# The editor loads games' code libraries (--game) to learn their component types. Those libraries call into the
# engine, so the editor exports it.
ph_export_engine(bowl-editor)

# On Windows a game's code library imports the engine from its own executable, so the editor can't load it. Each
# hot-reloadable game gets a second copy here, <game>_editor_code, linked against the editor instead.
if(WIN32)
    get_property(games GLOBAL PROPERTY PH_EDITOR_GAMES)
    foreach(game IN LISTS games)
        get_target_property(sources ${game}_code PH_GAME_SOURCES)
        add_library(${game}_editor_code MODULE ${sources})
        target_compile_definitions(${game}_editor_code PRIVATE PH_HOT_RELOAD_LIBRARY=1)
        target_link_libraries(${game}_editor_code PRIVATE $<COMPILE_ONLY:ph::engine> bowl-editor)
        ph_configure_target(${game}_editor_code)
        set_target_properties(${game}_editor_code PROPERTIES FOLDER tools)
    endforeach()
endif()
