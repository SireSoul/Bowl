# Bowl Editor: a separate program, built on the engine, for laying out levels (.world files).
# It needs the dev tools (Dear ImGui), so it's only built when PH_DEV_TOOLS is on.

add_executable(bowl-editor
    src/main.cpp
    src/document.cpp
    src/editor.cpp
    src/panels.cpp
    src/ui.cpp)
ph_add_game(bowl-editor)
set_target_properties(bowl-editor PROPERTIES FOLDER tools)
# The editor loads games' code libraries (--game) to learn their component types. Those libraries call into the
# engine, so the editor exports it. Not on Windows yet: there, a game library is tied to its own executable.
if(NOT WIN32)
    ph_export_engine(bowl-editor)
endif()
