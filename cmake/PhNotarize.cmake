# Script mode: has Apple notarize a signed .app, .dmg or .zip and staples the ticket to it, so Gatekeeper opens it on
# other Macs without a warning. Sign with a Developer ID first (PH_MACOS_SIGN_IDENTITY), and store your credentials
# once with `xcrun notarytool store-credentials <profile>`.
#
#     cmake -DFILE=dist/my_game.app -DPROFILE=<profile> -P <engine>/cmake/PhNotarize.cmake

foreach(var IN ITEMS FILE PROFILE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PhNotarize.cmake needs -D${var}=...")
    endif()
endforeach()
if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "${FILE} doesn't exist")
endif()

# notarytool takes .zip, .dmg and .pkg files; an .app goes up zipped.
set(upload "${FILE}")
if(FILE MATCHES "\\.app/?$")
    get_filename_component(app_name "${FILE}" NAME)
    set(upload "${CMAKE_CURRENT_BINARY_DIR}/${app_name}.notarize.zip")
    execute_process(COMMAND ditto -c -k --keepParent "${FILE}" "${upload}" RESULT_VARIABLE result)
    if(result)
        message(FATAL_ERROR "couldn't zip ${FILE}")
    endif()
endif()

execute_process(COMMAND xcrun notarytool submit "${upload}" --keychain-profile "${PROFILE}" --wait RESULT_VARIABLE result)
if(NOT upload STREQUAL FILE)
    file(REMOVE "${upload}")
endif()
if(result)
    message(FATAL_ERROR "notarization failed; `xcrun notarytool log <submission id> --keychain-profile ${PROFILE}` says why")
endif()

if(FILE MATCHES "\\.zip$")
    message(STATUS "Notarized. A .zip can't hold a stapled ticket; Gatekeeper looks it up online instead.")
    return()
endif()
execute_process(COMMAND xcrun stapler staple "${FILE}" RESULT_VARIABLE result)
if(result)
    message(FATAL_ERROR "couldn't staple the ticket to ${FILE}")
endif()
message(STATUS "Notarized and stapled: ${FILE}")
