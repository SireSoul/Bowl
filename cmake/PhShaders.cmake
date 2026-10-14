# Shaders are written once in Vulkan-flavoured GLSL (`name.vert.glsl`, `name.frag.glsl`) and compiled by ph-shaderc into
# a `.phsh` bundle holding SPIR-V (Vulkan) and MSL (Metal) plus the resource counts SDL_GPU needs. Dev builds also compile
# them at runtime, so editing a .glsl file updates the running game.

function(ph_shaderc_command out_var)
    if(PH_SHADERC_EXECUTABLE)
        set(${out_var} "${PH_SHADERC_EXECUTABLE}" PARENT_SCOPE)
    else()
        set(${out_var} "$<TARGET_FILE:ph-shaderc>" PARENT_SCOPE)
    endif()
endfunction()

# ph_compile_shaders(<outputs_var> <source_dir> <output_dir>)
# Adds one build rule per shader under <source_dir>; each writes <output_dir>/<relative path>.phsh.
function(ph_compile_shaders outputs_var source_dir output_dir)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
         "${source_dir}/*.vert.glsl" "${source_dir}/*.frag.glsl" "${source_dir}/*.comp.glsl")
    ph_shaderc_command(shaderc)
    set(tool_dep "")
    if(NOT PH_SHADERC_EXECUTABLE)
        set(tool_dep ph-shaderc)
    endif()

    set(outputs "")
    foreach(src IN LISTS sources)
        file(RELATIVE_PATH rel "${source_dir}" "${src}")
        string(REGEX REPLACE "\\.glsl$" ".phsh" rel_out "${rel}")
        set(out "${output_dir}/${rel_out}")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${shaderc} "${src}" -o "${out}" --depfile "${out}.d"
            DEPENDS "${src}" ${tool_dep}
            DEPFILE "${out}.d"
            COMMENT "Compiling shader ${rel}"
            VERBATIM)
        list(APPEND outputs "${out}")
    endforeach()
    set(${outputs_var} ${outputs} PARENT_SCOPE)
endfunction()

# ph_add_game(<target> [ASSETS <dir>])
# Links the engine and lays out the runtime folder next to the executable:
#   <exe dir>/engine/...  engine assets (compiled shaders)
#   <exe dir>/assets/...  the game's assets, with its shaders compiled
# Dev builds read assets straight from the source folders instead, so edits hot-reload.
# With PH_MACOS_BUNDLES (on in the release preset) a macOS game is a .app with these folders in Contents/Resources.
# `cmake --install <build dir> --prefix <folder>` lays out a shippable copy of every game.
#   BUNDLE_ID com.example.game   macOS bundle identifier (default: com.placeholderenginename.<target>)
function(ph_add_game target)
    cmake_parse_arguments(ARG "" "ASSETS;BUNDLE_ID" "" ${ARGN})
    target_link_libraries(${target} PRIVATE ph::engine)
    ph_configure_target(${target})

    set(bundle OFF)
    if(APPLE AND NOT IOS AND PH_MACOS_BUNDLES)
        set(bundle ON)
        if(NOT ARG_BUNDLE_ID)
            string(TOLOWER "com.placeholderenginename.${target}" ARG_BUNDLE_ID)
        endif()
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${target}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${ARG_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
        set(dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources")
    else()
        set(dest "$<TARGET_FILE_DIR:${target}>")
    endif()
    set(commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PH_ENGINE_ASSET_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PH_ENGINE_ASSET_SOURCE_DIR}" "${dest}/engine"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PH_ENGINE_ASSET_BUILD_DIR}" "${dest}/engine")
    set(shader_outputs "")
    if(ARG_ASSETS)
        get_filename_component(assets_abs "${ARG_ASSETS}" ABSOLUTE)
        set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders")
        ph_compile_shaders(shader_outputs "${assets_abs}" "${shader_dir}")
        list(APPEND commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${assets_abs}" "${dest}/assets"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${shader_dir}" "${dest}/assets")
        target_compile_definitions(${target} PRIVATE PH_GAME_ASSET_SOURCE_DIR="${assets_abs}")
    endif()

    add_custom_target(${target}_assets ${commands} DEPENDS ${shader_outputs} VERBATIM)
    add_dependencies(${target}_assets ph_engine_assets)
    add_dependencies(${target} ${target}_assets)
    set_target_properties(${target}_assets PROPERTIES FOLDER assets)

    # Install: the executable (or .app) plus its asset folders, ready to zip and ship.
    if(bundle)
        install(TARGETS ${target} BUNDLE DESTINATION .)
    else()
        install(TARGETS ${target} RUNTIME DESTINATION ${target})
        install(DIRECTORY "${dest}/engine" DESTINATION ${target})
        if(ARG_ASSETS)
            install(DIRECTORY "${dest}/assets" DESTINATION ${target})
        endif()
    endif()
endfunction()
