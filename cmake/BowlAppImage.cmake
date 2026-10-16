# Script mode, run by <game>_appimage (see bowl_package): installs the build into an AppDir and, when appimagetool is
# found (PATH, or -DAPPIMAGETOOL=<path>), packs it into one AppImage file.
#   -DBUILD_DIR=<build folder> -DCONFIG=<config> -DTARGET=<game> -DAPPDIR=<folder> -DOUTPUT=<file.AppImage>

foreach(var IN ITEMS BUILD_DIR TARGET APPDIR OUTPUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "BowlAppImage.cmake needs -D${var}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${APPDIR}")
set(config_args "")
if(CONFIG)
    set(config_args --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" ${config_args} --prefix "${APPDIR}" RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR "installing into ${APPDIR} failed")
endif()

# An AppImage runs AppRun, and takes its menu entry and icon from the top of the AppDir.
if(NOT EXISTS "${APPDIR}/${TARGET}/${TARGET}.png")
    message(FATAL_ERROR "${TARGET} has no icon: an AppImage needs one (bowl_add_game(${TARGET} ... ICON icon.png))")
endif()
file(WRITE "${APPDIR}/AppRun" "#!/bin/sh
here=\"$(dirname \"$(readlink -f \"$0\")\")\"
exec \"$here/${TARGET}/${TARGET}\" \"$@\"
")
file(CHMOD "${APPDIR}/AppRun" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
     WORLD_EXECUTE)
file(COPY "${APPDIR}/${TARGET}/${TARGET}.desktop" "${APPDIR}/${TARGET}/${TARGET}.png" DESTINATION "${APPDIR}")
file(CREATE_LINK "${TARGET}.png" "${APPDIR}/.DirIcon" SYMBOLIC)

if(NOT APPIMAGETOOL)
    find_program(APPIMAGETOOL NAMES appimagetool appimagetool-x86_64.AppImage appimagetool-aarch64.AppImage)
endif()
if(NOT APPIMAGETOOL)
    message(STATUS "The AppDir is ready: ${APPDIR}")
    message(STATUS "To pack it, get appimagetool (https://github.com/AppImage/appimagetool/releases) and run this again "
                   "with it on PATH, or run: appimagetool \"${APPDIR}\" \"${OUTPUT}\"")
    return()
endif()
execute_process(COMMAND "${APPIMAGETOOL}" "${APPDIR}" "${OUTPUT}" RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR "appimagetool failed")
endif()
message(STATUS "Made ${OUTPUT}")
