# Shaders are written once in Vulkan-flavoured GLSL (`name.vert.glsl`, `name.frag.glsl`) and compiled by bowl-shaderc
# into a `.bowlsh` bundle holding SPIR-V (Vulkan), MSL (Metal) and HLSL (Direct3D 12) plus the resource counts SDL_GPU
# needs.
# Dev builds also compile them at runtime, so editing a .glsl file updates the running game.

function(bowl_shaderc_command out_var)
    if(BOWL_SHADERC_EXECUTABLE)
        set(${out_var} "${BOWL_SHADERC_EXECUTABLE}" PARENT_SCOPE)
    else()
        set(${out_var} "$<TARGET_FILE:bowl-shaderc>" PARENT_SCOPE)
    endif()
endfunction()

# bowl_compile_shaders(<outputs_var> <source_dir> <output_dir>)
# Adds one build rule per shader under <source_dir>; each writes <output_dir>/<relative path>.bowlsh.
function(bowl_compile_shaders outputs_var source_dir output_dir)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
         "${source_dir}/*.vert.glsl" "${source_dir}/*.frag.glsl" "${source_dir}/*.comp.glsl")
    bowl_shaderc_command(shaderc)
    set(tool_dep "")
    if(NOT BOWL_SHADERC_EXECUTABLE)
        set(tool_dep bowl-shaderc)
    endif()

    set(outputs "")
    foreach(src IN LISTS sources)
        file(RELATIVE_PATH rel "${source_dir}" "${src}")
        string(REGEX REPLACE "\\.glsl$" ".bowlsh" rel_out "${rel}")
        set(out "${output_dir}/${rel_out}")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${shaderc} "${src}" -o "${out}" --depfile "${out}.d"
                    --include "engine/shaders/=${BOWL_ENGINE_ASSET_SOURCE_DIR}/shaders"
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
function(bowl_make_hot_reloadable target)
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
    target_compile_definitions(${code} PRIVATE BOWL_HOT_RELOAD_LIBRARY=1)
    target_link_libraries(${code} PRIVATE $<COMPILE_ONLY:bowl::engine>)
    bowl_configure_target(${code})
    if(APPLE)
        target_link_options(${code} PRIVATE "LINKER:-undefined,dynamic_lookup" "LINKER:-no_fixup_chains")
    endif()
    set_target_properties(${code} PROPERTIES FOLDER samples)

    set(host_sources "${BOWL_ENGINE_SOURCE_DIR}/app/hot/host_main.cpp")
    set(engine_libs bowl_core bowl_ecs bowl_platform bowl_gfx bowl_audio bowl_physics bowl_ui bowl_app bowl_imgui)
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
        BOWL_HOT_RELOAD_LIBRARY_PATH="$<TARGET_FILE:${code}>"
        BOWL_HOT_RELOAD_SOURCE_DIRS="${source_dirs}"
        BOWL_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
        BOWL_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
        BOWL_HOT_RELOAD_TARGET="${code}"
        BOWL_HOT_RELOAD_CONFIG="$<CONFIG>")
    if(WIN32)
        # The library links against the host, so the host can't also depend on the library. Building everything (or
        # the library) builds both.
        # Bowl Editor builds a second copy linked against itself (see editor/CMakeLists.txt), since a Windows DLL is
        # tied to the executable it imports the engine from.
        set_property(GLOBAL APPEND PROPERTY BOWL_EDITOR_GAMES ${target})
        set_property(TARGET ${code} PROPERTY BOWL_GAME_SOURCES "${abs_sources}")
        return()
    endif()
    bowl_export_engine(${target})
    add_dependencies(${target} ${code})
endfunction()

# bowl_export_engine(<target>): links every engine object into an executable and exports them, so libraries it loads at
# runtime (a game's code library) can call into the engine. The editor uses this to load a game's components.
function(bowl_export_engine target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(WIN32)
        # WINDOWS_EXPORT_ALL_SYMBOLS only exports a target's own object files, so the engine's objects are built in
        # (the same set the hot-reload host builds in).
        foreach(lib bowl_core bowl_ecs bowl_platform bowl_gfx bowl_audio bowl_physics bowl_ui bowl_app bowl_imgui)
            if(TARGET ${lib})
                target_sources(${target} PRIVATE $<TARGET_OBJECTS:${lib}>)
            endif()
        endforeach()
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        return()
    endif()
    foreach(lib bowl_core bowl_ecs bowl_platform bowl_gfx bowl_audio bowl_physics bowl_ui bowl_app bowl_imgui
                bowl_shaderc)
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

# bowl_add_game(<target> [ASSETS <dir>] [NAME <name>] [ICON <png>] [ICON_PIXEL_ART] [BUNDLE_ID <id>] [HOT_RELOAD])
# Links the engine and lays out the runtime folder next to the executable:
#   <exe dir>/engine/...  engine assets (compiled shaders)
#   <exe dir>/assets/...  the game's assets, with its shaders compiled
#   <exe dir>/bowl-LICENSE.txt, bowl-NOTICE.txt  Bowl's license and notice, which every copy of the game carries
# Dev builds read assets straight from the source folders instead, so edits hot-reload.
# With BOWL_MACOS_BUNDLES (on in the release preset) a macOS game is a .app with these folders in Contents/Resources.
# `cmake --install <build dir> --prefix <folder>` lays out a shippable copy of every game (signed, see
# BowlPackaging.cmake); bowl_package adds installers.
#   NAME "My Game"               the name players see: macOS bundle, Linux menus, installers (default: <target>)
#   ICON art/icon.png            the application icon: a square PNG, 1024 x 1024 at best. Becomes the .icns, .ico or
#                                Linux icon, and the window's icon
#   ICON_PIXEL_ART               scale the icon with hard pixel edges
#   BUNDLE_ID com.example.game   macOS bundle identifier, Flatpak app id (default: com.bowl.<target>)
#   HOT_RELOAD                   dev builds: the game's sources become <target>_code, a library the
#                                <target> executable reloads whenever it's rebuilt (see bowl/app/hot_reload.hpp)
function(bowl_add_game target)
    cmake_parse_arguments(ARG "HOT_RELOAD;ICON_PIXEL_ART" "ASSETS;BUNDLE_ID;NAME;ICON;ORIENTATION" "" ${ARGN})
    target_link_libraries(${target} PRIVATE bowl::engine)
    bowl_configure_target(${target})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target}")
    endif()
    if(NOT ARG_BUNDLE_ID)
        string(TOLOWER "com.bowl.${target}" ARG_BUNDLE_ID)
    endif()
    set_target_properties(${target} PROPERTIES BOWL_DISPLAY_NAME "${ARG_NAME}" BOWL_BUNDLE_ID "${ARG_BUNDLE_ID}")

    if(ARG_HOT_RELOAD AND BOWL_DEV_TOOLS)
        if(CMAKE_VERSION VERSION_LESS 3.27)
            message(WARNING "${target}: hot reload needs CMake 3.27+; building it normally")
        else()
            bowl_make_hot_reloadable(${target})
        endif()
    endif()

    set(bundle OFF)
    if(IOS OR (APPLE AND BOWL_MACOS_BUNDLES))
        set(bundle ON)
    endif()
    if(ARG_ICON)
        _bowl_game_icon(${target} "${ARG_ICON}" "${ARG_ICON_PIXEL_ART}" ${bundle})
    endif()
    if(IOS)
        # An iOS app: everything at the top of the bundle, which Xcode signs (ad hoc for the simulator; BOWL_IOS_TEAM
        # for devices).
        if(NOT ARG_ORIENTATION)
            set(ARG_ORIENTATION landscape)
        endif()
        set(BOWL_IOS_ORIENTATIONS "")
        if(ARG_ORIENTATION STREQUAL "landscape" OR ARG_ORIENTATION STREQUAL "any")
            string(APPEND BOWL_IOS_ORIENTATIONS "<string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>")
        endif()
        if(ARG_ORIENTATION STREQUAL "portrait" OR ARG_ORIENTATION STREQUAL "any")
            string(APPEND BOWL_IOS_ORIENTATIONS "<string>UIInterfaceOrientationPortrait</string>")
        endif()
        if(ARG_ICON)
            set(BOWL_IOS_ICON "<key>CFBundleIconName</key><string>AppIcon</string>")
        else()
            set(BOWL_IOS_ICON "")
        endif()
        configure_file("${BOWL_CMAKE_DIR}/Info-iOS.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist.in"
            @ONLY)
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${ARG_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${ARG_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${ARG_BUNDLE_ID}"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2") # iPhone and iPad
        if(BOWL_IOS_TEAM)
            set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BOWL_IOS_TEAM}"
                                                       XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
        else()
            set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
        endif()
        set(dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>")
    elseif(bundle)
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${ARG_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${ARG_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
        set(dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources")
    else()
        set(dest "$<TARGET_FILE_DIR:${target}>")
    endif()
    set(commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BOWL_ENGINE_ASSET_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${BOWL_ENGINE_ASSET_SOURCE_DIR}" "${dest}/engine"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${BOWL_ENGINE_ASSET_BUILD_DIR}" "${dest}/engine")
    # Bowl's license and notice travel with every copy of the game (Apache License 2.0). They sit beside the asset
    # folders, so packing leaves them readable.
    foreach(file IN LISTS BOWL_LICENSE_FILES)
        get_filename_component(name "${file}" NAME)
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "${dest}/bowl-${name}.txt")
    endforeach()
    set(shader_outputs "")
    if(ARG_ASSETS)
        get_filename_component(assets_abs "${ARG_ASSETS}" ABSOLUTE)
        set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders")
        bowl_compile_shaders(shader_outputs "${assets_abs}" "${shader_dir}")
        list(APPEND commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${assets_abs}" "${dest}/assets"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${shader_dir}" "${dest}/assets")
        target_compile_definitions(${target} PRIVATE BOWL_GAME_ASSET_SOURCE_DIR="${assets_abs}")
    endif()
    # Shipping builds can pack each folder into one archive (engine.bowlpak, assets.bowlpak) that the engine reads
    # instead.
    set(folders engine)
    if(ARG_ASSETS)
        list(APPEND folders assets)
    endif()
    # Each is cooked in place first (BOWL_COOK_ASSETS): binary JSON, flattened maps and BWTX pictures, at the same paths.
    if(BOWL_PACK_ASSETS)
        bowl_host_tool(bowl_pack bowl-pack)
        bowl_host_tool(bowl_cook bowl-cook)
        foreach(folder IN LISTS folders)
            if(BOWL_COOK_ASSETS)
                list(APPEND commands COMMAND ${bowl_cook} "${dest}/${folder}" "${dest}/${folder}")
            endif()
            list(APPEND commands
                COMMAND ${bowl_pack} "${dest}/${folder}" -o "${dest}/${folder}.bowlpak"
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${dest}/${folder}")
        endforeach()
    endif()

    add_custom_target(${target}_assets ${commands} DEPENDS ${shader_outputs} VERBATIM)
    add_dependencies(${target}_assets bowl_engine_assets)
    if(BOWL_PACK_ASSETS AND NOT BOWL_HOST_TOOLS)
        add_dependencies(${target}_assets bowl-pack)
        if(BOWL_COOK_ASSETS)
            add_dependencies(${target}_assets bowl-cook)
        endif()
    endif()
    add_dependencies(${target} ${target}_assets)
    set_target_properties(${target}_assets PROPERTIES FOLDER assets)

    # Install: the executable (or .app) plus its asset folders or archives, ready to zip and ship. Everything a game
    # installs is in its own component, so its installers (bowl_package) carry only that game.
    if(bundle)
        install(TARGETS ${target} BUNDLE DESTINATION . COMPONENT ${target})
    else()
        install(TARGETS ${target} RUNTIME DESTINATION ${target} COMPONENT ${target})
        foreach(folder IN LISTS folders)
            if(BOWL_PACK_ASSETS)
                install(FILES "${dest}/${folder}.bowlpak" DESTINATION ${target} COMPONENT ${target})
            else()
                install(DIRECTORY "${dest}/${folder}" DESTINATION ${target} COMPONENT ${target})
            endif()
        endforeach()
        foreach(file IN LISTS BOWL_LICENSE_FILES)
            get_filename_component(name "${file}" NAME)
            install(FILES "${file}" DESTINATION ${target} RENAME "bowl-${name}.txt" COMPONENT ${target})
        endforeach()
        if(NOT APPLE AND NOT WIN32)
            if(ARG_ICON)
                set(has_icon ON)
            else()
                set(has_icon OFF)
            endif()
            _bowl_linux_desktop(${target} "${ARG_NAME}" "${ARG_BUNDLE_ID}" ${has_icon})
        endif()
    endif()
    _bowl_sign_installed(${target} ${bundle})
endfunction()
