# bowl-rendezvous: the server players meet through for net::Host rooms (introductions, NAT punch-through, relay)

add_executable(bowl-rendezvous src/main.cpp)
target_link_libraries(bowl-rendezvous PRIVATE bowl_net)
bowl_configure_target(bowl-rendezvous)
set_target_properties(bowl-rendezvous PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")
