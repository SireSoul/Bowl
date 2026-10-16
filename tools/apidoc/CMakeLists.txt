# bowl_apidoc: the API reference from the engine's public headers (parsing and Markdown, testable)
# bowl-apidoc: the command-line tool around it
# bowl_api_docs: regenerates docs/reference/api

add_library(bowl_apidoc STATIC src/apidoc.cpp)
add_library(bowl::apidoc ALIAS bowl_apidoc)
target_include_directories(bowl_apidoc PUBLIC include)
target_link_libraries(bowl_apidoc PUBLIC bowl_core)
bowl_configure_target(bowl_apidoc)
set_target_properties(bowl_apidoc PROPERTIES FOLDER tools)

add_executable(bowl-apidoc src/main.cpp)
target_link_libraries(bowl-apidoc PRIVATE bowl_apidoc)
bowl_configure_target(bowl-apidoc)
set_target_properties(bowl-apidoc PROPERTIES FOLDER tools RUNTIME_OUTPUT_DIRECTORY "${BOWL_TOOLS_DIR}")

add_custom_target(bowl_api_docs
    COMMAND bowl-apidoc "${BOWL_ENGINE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/docs/reference/api"
    COMMENT "Writing the API reference"
    VERBATIM)
set_target_properties(bowl_api_docs PROPERTIES FOLDER tools)
