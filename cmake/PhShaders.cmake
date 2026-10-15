# Shaders are written once in Vulkan-flavoured GLSL (`name.vert.glsl`, `name.frag.glsl`) and compiled by ph-shaderc into
# a `.phsh` bundle holding SPIR-V (Vulkan), MSL (Metal) and HLSL (Direct3D 12) plus the resource counts SDL_GPU needs.
# Dev builds also compile them at runtime, so editing a .glsl file updates the running game.

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

# Moves a game's sources into a loadable library (<target>_code) and turns <target> into the engine host that loads
# and reloads it. The host contains every engine object file and exports its symbols. On macOS and Linux the library
# links against nothing and finds engine functions in the host when it's loaded. Windows has no such lazy lookup, so
# there the library links against the host's import library instead.
function(ph_make_hot_reloadable target)
    get_target_property(sources ${target} SOURCES)
    get_target_property(source_dir ${target} SOURCE_DIR)
    set(abs_sources "")
    set(source_dirs "")
    foreach(s IN LISTS sources)
        if(NOT IS_ABSOLUTE "${s}")
            set(s "${source_dir}/${s}")
        endif()
        list(APPEND abs_sources "${s}")
        get_filename_component(dir "${s}" DIRECTORY)
        list(APPEND source_dirs "${dir}")
    endforeach()
    list(REMOVE_DUPLICATES source_dirs)
    string(REPLACE ";" "|" source_dirs "${source_dirs}")

    set(code ${target}_code)
    add_library(${code} MODULE ${abs_sources})
    target_compile_definitions(${code} PRIVATE PH_HOT_RELOAD_LIBRARY=1)
    target_link_libraries(${code} PRIVATE $<COMPILE_ONLY:ph::engine>)
    ph_configure_target(${code})
    if(APPLE)
        target_link_options(${code} PRIVATE "LINKER:-undefined,dynamic_lookup" "LINKER:-no_fixup_chains")
    endif()
    set_target_properties(${code} PROPERTIES FOLDER samples)

    set(host_sources "${PH_ENGINE_SOURCE_DIR}/app/hot/host_main.cpp")
    set(engine_libs ph_core ph_ecs ph_platform ph_gfx ph_audio ph_physics ph_ui ph_app ph_imgui)
    if(WIN32)
        # WINDOWS_EXPORT_ALL_SYMBOLS only exports a target's own object files, so the engine's objects are built
        # into the host directly (the static libraries it also links then have nothing left to contribute).
        foreach(lib IN LISTS engine_libs)
            if(TARGET ${lib})
                list(APPEND host_sources $<TARGET_OBJECTS:${lib}>)
            endif()
        endforeach()
        set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
        target_link_libraries(${code} PRIVATE ${target})
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES ${host_sources})
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # The host rebuilds the library itself when a source file is saved: it watches the folders holding the sources
    # and runs this build.
    target_compile_definitions(${target} PRIVATE
        PH_HOT_RELOAD_LIBRARY_PATH="$<TARGET_FILE:${code}>"
        PH_HOT_RELOAD_SOURCE_DIRS="${source_dirs}"
        PH_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
        PH_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
        PH_HOT_RELOAD_TARGET="${code}"
        PH_HOT_RELOAD_CONFIG="$<CONFIG>")
    if(WIN32)
        # The library links against the host, so the host can't also depend on the library. Building everything (or
        # the library) builds both.
        # Bowl Editor builds a second copy linked against itself (see editor/CMakeLists.txt), since a Windows DLL is
        # tied to the executable it imports the engine from.
        set_property(GLOBAL APPEND PROPERTY PH_EDITOR_GAMES ${target})
        set_property(TARGET ${code} PROPERTY PH_GAME_SOURCES "${abs_sources}")
        return()
    endif()
    ph_export_engine(${target})
    add_dependencies(${target} ${code})
endfunction()

# ph_export_engine(<target>): links every engine object into an executable and exports them, so libraries it loads at
# runtime (a game's code library) can call into the engine. The editor uses this to load a game's components.
function(ph_export_engine target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(WIN32)
        # WINDOWS_EXPORT_ALL_SYMBOLS only exports a target's own object files, so the engine's objects are built in
        # (the same set the hot-reload host builds in).
        foreach(lib ph_core ph_ecs ph_platform ph_gfx ph_audio ph_physics ph_ui ph_app ph_imgui)
            if(TARGET ${lib})
                target_sources(${target} PRIVATE $<TARGET_OBJECTS:${lib}>)
            endif()
        endforeach()
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        return()
    endif()
    foreach(lib ph_core ph_ecs ph_platform ph_gfx ph_audio ph_physics ph_ui ph_app ph_imgui ph_shaderc)
        if(TARGET ${lib})
            if(APPLE)
                target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:${lib}>")
            else()
                target_link_options(${target} PRIVATE "LINKER:--whole-archive" "$<TARGET_FILE:${lib}>"
                                                      "LINKER:--no-whole-archive")
            endif()
        endif()
    endforeach()
endfunction()

# ph_add_game(<target> [ASSETS <dir>] [BUNDLE_ID <id>] [HOT_RELOAD])
# Links the engine and lays out the runtime folder next to the executable:
#   <exe dir>/engine/...  engine assets (compiled shaders)
#   <exe dir>/assets/...  the game's assets, with its shaders compiled
# Dev builds read assets straight from the source folders instead, so edits hot-reload.
# With PH_MACOS_BUNDLES (on in the release preset) a macOS game is a .app with these folders in Contents/Resources.
# `cmake --install <build dir> --prefix <folder>` lays out a shippable copy of every game.
#   BUNDLE_ID com.example.game   macOS bundle identifier (default: com.bowl.<target>)
#   HOT_RELOAD                   dev builds: the game's sources become <target>_code, a library the
#                                <target> executable reloads whenever it's rebuilt (see ph/app/hot_reload.hpp)
function(ph_add_game target)
    cmake_parse_arguments(ARG "HOT_RELOAD" "ASSETS;BUNDLE_ID" "" ${ARGN})
    target_link_libraries(${target} PRIVATE ph::engine)
    ph_configure_target(${target})

    if(ARG_HOT_RELOAD AND PH_DEV_TOOLS)
        if(CMAKE_VERSION VERSION_LESS 3.27)
            message(WARNING "${target}: hot reload needs CMake 3.27+; building it normally")
        else()
            ph_make_hot_reloadable(${target})
        endif()
    endif()

    set(bundle OFF)
    if(APPLE AND NOT IOS AND PH_MACOS_BUNDLES)
        set(bundle ON)
        if(NOT ARG_BUNDLE_ID)
            string(TOLOWER "com.bowl.${target}" ARG_BUNDLE_ID)
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
