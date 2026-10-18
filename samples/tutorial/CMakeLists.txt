add_executable(tutorial main.cpp)
bowl_add_game(tutorial ASSETS assets NAME "Bowl Tutorial" ICON ../icon.png ICON_PIXEL_ART)
set_target_properties(tutorial PROPERTIES FOLDER samples)
