add_executable(isometric main.cpp)
ph_add_game(isometric NAME "Bowl Isometric" ICON ../icon.png ICON_PIXEL_ART)
set_target_properties(isometric PROPERTIES FOLDER samples)
