# ph_apidoc: the API reference from the engine's public headers (parsing and Markdown, testable)
# ph-apidoc: the command-line tool around it
# ph_api_docs: regenerates docs/reference/api

add_library(ph_apidoc STATIC src/apidoc.cpp)
add_library(ph::apidoc ALIAS ph_apidoc)
target_include_directories(ph_apidoc PUBLIC include)
target_link_libraries(ph_apidoc PUBLIC ph_core)
ph_configure_target(ph_apidoc)
set_target_properties(ph_apidoc PROPERTIES FOLDER tools)

add_executable(ph-apidoc src/main.cpp)
target_link_libraries(ph-apidoc PRIVATE ph_apidoc)
ph_configure_target(ph-apidoc)
set_target_properties(ph-apidoc PROPERTIES FOLDER tools)

add_custom_target(ph_api_docs
    COMMAND ph-apidoc "${PH_ENGINE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/docs/reference/api"
    COMMENT "Writing the API reference"
    VERBATIM)
set_target_properties(ph_api_docs PROPERTIES FOLDER tools)
