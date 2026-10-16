add_executable(sprites main.cpp)
bowl_add_game(sprites NAME "Bowl Sprites" ASSETS assets ICON ../icon.png ICON_PIXEL_ART HOT_RELOAD)
set_target_properties(sprites PROPERTIES FOLDER samples)
