# Shipping: icons, signing and installers for games made with bowl_add_game (see docs/guides/packaging.md).

set(BOWL_MACOS_SIGN_IDENTITY "-" CACHE STRING
    "macOS: codesign identity for installed .app bundles (\"-\": ad hoc, \"Developer ID Application: ...\" to ship, empty: don't sign)")
set(BOWL_MACOS_ENTITLEMENTS "" CACHE FILEPATH "macOS: entitlements file for codesign (optional)")
set(BOWL_WINDOWS_SIGN_COMMAND "" CACHE STRING
    "Windows: a command run on each installed .exe to sign it, as a list (e.g. signtool;sign;/fd;SHA256;/a;/tr;http://timestamp.digicert.com;/td;SHA256)")
set(BOWL_IOS_TEAM "" CACHE STRING "iOS: Apple development team ID that signs apps for devices (empty: ad hoc, simulator only)")
option(BOWL_LINUX_SHARE "Linux: also install the desktop file and icon under share/, and the game under bin/ (Flatpak, distro packages)" OFF)

set(BOWL_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

# Quotes each item so it survives being written into install(CODE).
function(_bowl_quote out_var)
    set(quoted "")
    foreach(item IN LISTS ARGN)
        string(APPEND quoted " [==[${item}]==]")
    endforeach()
    set(${out_var} "${quoted}" PARENT_SCOPE)
endfunction()

# bowl_host_tool(<out_var> <name>): the command for a build tool (bowl-icon, bowl-pack): the host's copy when
# cross-compiling (BOWL_HOST_TOOLS), otherwise the one this build makes.
function(bowl_host_tool out_var name)
    if(BOWL_HOST_TOOLS)
        set(${out_var} "${BOWL_HOST_TOOLS}/${name}${CMAKE_HOST_EXECUTABLE_SUFFIX}" PARENT_SCOPE)
    else()
        set(${out_var} ${name} PARENT_SCOPE)
    endif()
endfunction()

# The game's icon in every form its platform wants, and in the executable for the window (bowl::embedded_icon).
function(_bowl_game_icon target icon pixel_art bundle)
    get_filename_component(icon_abs "${icon}" ABSOLUTE)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon")
    file(MAKE_DIRECTORY "${dir}")
    bowl_host_tool(bowl_icon bowl-icon)
    set(args "${icon_abs}")
    set(outputs "")
    if(NOT IOS) # iOS windows have no icon of their own
        list(APPEND args --cpp "${dir}/icon.cpp")
        list(APPEND outputs "${dir}/icon.cpp")
        target_sources(${target} PRIVATE "${dir}/icon.cpp")
    endif()
    if(pixel_art)
        list(APPEND args --pixel-art)
    endif()
    if(IOS)
        # An asset catalog with one 1024 x 1024 picture; Xcode makes every size from it.
        set(catalog "${dir}/Assets.xcassets")
        set(platform_icon "${catalog}/AppIcon.appiconset/icon-1024.png")
        file(MAKE_DIRECTORY "${catalog}/AppIcon.appiconset")
        file(CONFIGURE OUTPUT "${catalog}/Contents.json" CONTENT "{ \"info\": { \"author\": \"bowl\", \"version\": 1 } }\n")
        file(CONFIGURE OUTPUT "${catalog}/AppIcon.appiconset/Contents.json" CONTENT [=[{
  "images": [{ "filename": "icon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" }],
  "info": { "author": "bowl", "version": 1 }
}
]=])
        list(APPEND args --png 1024 "${platform_icon}")
    elseif(APPLE)
        set(platform_icon "${dir}/${target}.icns")
        list(APPEND args --icns "${platform_icon}")
    elseif(WIN32)
        set(platform_icon "${dir}/${target}.ico")
        list(APPEND args --ico "${platform_icon}")
    else()
        set(platform_icon "${dir}/${target}.png")
        list(APPEND args --png 256 "${platform_icon}")
    endif()
    list(APPEND outputs "${platform_icon}")
    add_custom_command(OUTPUT ${outputs} COMMAND ${bowl_icon} ${args} DEPENDS ${bowl_icon} "${icon_abs}"
        COMMENT "Making ${target}'s icons" VERBATIM)
    set_target_properties(${target} PROPERTIES BOWL_PLATFORM_ICON "${platform_icon}")

    if(IOS)
        target_sources(${target} PRIVATE "${platform_icon}" "${catalog}")
        set_source_files_properties("${catalog}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
    elseif(APPLE AND bundle)
        target_sources(${target} PRIVATE "${platform_icon}")
        set_source_files_properties("${platform_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE "${target}.icns")
    elseif(WIN32)
        # The icon Explorer shows for the .exe.
        file(CONFIGURE OUTPUT "${dir}/icon.rc" CONTENT "1 ICON \"${platform_icon}\"\n")
        target_sources(${target} PRIVATE "${dir}/icon.rc")
        set_source_files_properties("${dir}/icon.rc" PROPERTIES OBJECT_DEPENDS "${platform_icon}")
    endif()
endfunction()

# Linux: a desktop file (for AppImage, Flatpak, menus) next to the game, and under share/ with BOWL_LINUX_SHARE.
function(_bowl_linux_desktop target display_name bundle_id has_icon)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon")
    foreach(kind IN ITEMS local share)
        if(kind STREQUAL "local")
            set(icon_name "${target}")
        else()
            set(icon_name "${bundle_id}")
        endif()
        file(CONFIGURE OUTPUT "${dir}/${kind}.desktop" CONTENT
"[Desktop Entry]
Type=Application
Name=${display_name}
Exec=${target}
Icon=${icon_name}
Terminal=false
Categories=Game;
")
    endforeach()
    install(FILES "${dir}/local.desktop" DESTINATION ${target} RENAME "${target}.desktop")
    if(has_icon)
        install(FILES "${dir}/${target}.png" DESTINATION ${target})
    endif()
    if(BOWL_LINUX_SHARE)
        install(FILES "${dir}/share.desktop" DESTINATION share/applications RENAME "${bundle_id}.desktop")
        if(has_icon)
            install(FILES "${dir}/${target}.png" DESTINATION share/icons/hicolor/256x256/apps RENAME "${bundle_id}.png")
        endif()
        # bin/<game> runs the copy that has its asset folders beside it.
        install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
file(CREATE_LINK \"../${target}/${target}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/${target}\" SYMBOLIC)")
    endif()
endfunction()

# Signs the installed game: codesign on macOS bundles, BOWL_WINDOWS_SIGN_COMMAND on Windows.
function(_bowl_sign_installed target bundle)
    if(APPLE AND NOT IOS AND bundle AND BOWL_MACOS_SIGN_IDENTITY) # Xcode signs iOS apps (BOWL_IOS_TEAM)
        set(args codesign --force --sign "${BOWL_MACOS_SIGN_IDENTITY}")
        if(NOT BOWL_MACOS_SIGN_IDENTITY STREQUAL "-")
            list(APPEND args --options runtime --timestamp) # what notarization asks for
        endif()
        if(BOWL_MACOS_ENTITLEMENTS)
            list(APPEND args --entitlements "${BOWL_MACOS_ENTITLEMENTS}")
        endif()
        _bowl_quote(command ${args})
        install(CODE "execute_process(COMMAND ${command} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${target}.app\"
    RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR
        \"codesign couldn't sign ${target}.app (BOWL_MACOS_SIGN_IDENTITY: ${BOWL_MACOS_SIGN_IDENTITY})\")
endif()")
    elseif(WIN32 AND BOWL_WINDOWS_SIGN_COMMAND)
        _bowl_quote(command ${BOWL_WINDOWS_SIGN_COMMAND})
        install(CODE "execute_process(COMMAND ${command} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${target}/${target}.exe\"
    RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR \"couldn't sign ${target}.exe with BOWL_WINDOWS_SIGN_COMMAND\")
endif()")
    endif()
endfunction()

# bowl_package(<target> [VENDOR <name>] [CONTACT <email>] [DESCRIPTION <text>] [LICENSE <file>])
# Installers for the game, made with `cpack` in the build folder (after building it):
#   macOS    a .dmg with the .app to drag to Applications, and a .zip
#   Windows  an NSIS installer (when makensis is installed: https://nsis.sourceforge.io) and a .zip
#   Linux    a .tar.gz; `cmake --build <build> --target <target>_appimage` makes an AppImage, and
#            <build>/<bundle id>.flatpak.json is a manifest for flatpak-builder
# Call it once, for the game the installers are for; the name, version, icon and bundle id come from bowl_add_game and
# project().
function(bowl_package target)
    cmake_parse_arguments(ARG "" "VENDOR;CONTACT;DESCRIPTION;LICENSE" "" ${ARGN})
    get_property(done GLOBAL PROPERTY BOWL_PACKAGED)
    if(done)
        message(FATAL_ERROR "bowl_package(${target}): only one game per build can have installers (${done} has them)")
    endif()
    set_property(GLOBAL PROPERTY BOWL_PACKAGED ${target})

    get_target_property(name ${target} BOWL_DISPLAY_NAME)
    get_target_property(bundle_id ${target} BOWL_BUNDLE_ID)
    get_target_property(icon ${target} BOWL_PLATFORM_ICON)
    if(NOT ARG_VENDOR)
        set(ARG_VENDOR "${name}")
    endif()
    set(version "${PROJECT_VERSION}")
    if(NOT version)
        set(version "0.0.0")
    endif()
    string(REGEX REPLACE "[^A-Za-z0-9._-]" "" file_name "${name}")

    set(CPACK_PACKAGE_NAME "${name}")
    set(CPACK_PACKAGE_VENDOR "${ARG_VENDOR}")
    set(CPACK_PACKAGE_VERSION "${version}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${ARG_DESCRIPTION}")
    set(CPACK_PACKAGE_CONTACT "${ARG_CONTACT}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${name}")
    set(CPACK_PACKAGE_FILE_NAME "${file_name}-${version}-${CMAKE_SYSTEM_NAME}")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    if(ARG_LICENSE)
        get_filename_component(CPACK_RESOURCE_FILE_LICENSE "${ARG_LICENSE}" ABSOLUTE)
    endif()

    if(APPLE)
        set(CPACK_GENERATOR DragNDrop ZIP)
        set(CPACK_DMG_VOLUME_NAME "${name}")
        set(CPACK_DMG_FORMAT UDZO)
        if(icon)
            set(CPACK_PACKAGE_ICON "${icon}") # the mounted volume's icon
        endif()
    elseif(WIN32)
        set(CPACK_GENERATOR ZIP)
        find_program(BOWL_MAKENSIS makensis PATHS "$ENV{ProgramFiles}/NSIS" "$ENV{ProgramFiles\(x86\)}/NSIS")
        if(BOWL_MAKENSIS)
            list(APPEND CPACK_GENERATOR NSIS)
        endif()
        set(CPACK_PACKAGE_EXECUTABLES "${target}" "${name}") # Start menu shortcut
        set(CPACK_NSIS_EXECUTABLES_DIRECTORY "${target}")
        set(CPACK_NSIS_DISPLAY_NAME "${name}")
        set(CPACK_NSIS_PACKAGE_NAME "${name}")
        set(CPACK_NSIS_INSTALLED_ICON_NAME "${target}\\\\${target}.exe")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        if(icon)
            set(CPACK_NSIS_MUI_ICON "${icon}")
            set(CPACK_NSIS_MUI_UNIICON "${icon}")
        endif()
    else()
        set(CPACK_GENERATOR TGZ)
        add_custom_target(${target}_appimage
            COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DCONFIG=$<CONFIG> -DTARGET=${target}
                    -DAPPDIR=${CMAKE_BINARY_DIR}/${target}.AppDir
                    -DOUTPUT=${CMAKE_BINARY_DIR}/${file_name}-${version}-${CMAKE_SYSTEM_PROCESSOR}.AppImage
                    -P ${BOWL_CMAKE_DIR}/BowlAppImage.cmake
            DEPENDS ${target}
            COMMENT "Making ${target}'s AppImage"
            VERBATIM)
        file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/${bundle_id}.flatpak.json" CONTENT [=[{
  "id": "@bundle_id@",
  "runtime": "org.freedesktop.Platform",
  "runtime-version": "24.08",
  "sdk": "org.freedesktop.Sdk",
  "command": "@target@",
  "finish-args": ["--socket=wayland", "--socket=fallback-x11", "--share=ipc", "--device=all", "--socket=pulseaudio"],
  "modules": [
    {
      "name": "@target@",
      "buildsystem": "cmake-ninja",
      "config-opts": ["-DCMAKE_BUILD_TYPE=Release", "-DBOWL_DEV_TOOLS=OFF", "-DBOWL_FETCH_SDL=ON",
                      "-DBOWL_LINUX_SHARE=ON", "-DBOWL_BUILD_TESTS=OFF", "-DBOWL_BUILD_EDITOR=OFF"],
      "build-options": { "build-args": ["--share=network"] },
      "sources": [{ "type": "dir", "path": "@CMAKE_SOURCE_DIR@" }]
    }
  ]
}
]=] @ONLY)
    endif()
    include(CPack)
endfunction()
