add_executable(isometric main.cpp)
bowl_add_game(isometric NAME "Bowl Isometric" ICON ../icon.png ICON_PIXEL_ART)
# Installers (cpack), an AppImage and a Flatpak manifest; the Package workflow builds and runs them.
bowl_package(isometric VENDOR "Starlight (SireSoul)" DESCRIPTION "A walk around an isometric map, made with Bowl"
             LICENSE ../../LICENSE)
set_target_properties(isometric PROPERTIES FOLDER samples)
