add_executable(sprites main.cpp)
ph_add_game(sprites ASSETS assets HOT_RELOAD)
set_target_properties(sprites PROPERTIES FOLDER samples)
