# Compiler settings shared by every engine, tool, test and game target.

# ph_configure_target(<target>): warnings, defines and platform flags for code we own.
function(ph_configure_target target)
    # PUBLIC, so a game that adds the engine with add_subdirectory compiles engine headers as C++20 too.
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(PH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                                                 -Wno-missing-field-initializers)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # GCC's -Wshadow also flags lambda locals that shadow the enclosing function's; match it.
            target_compile_options(${target} PRIVATE -Wshadow-uncaptured-local)
        endif()
        if(PH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(PH_DEV_TOOLS)
        target_compile_definitions(${target} PUBLIC PH_DEV_TOOLS=1)
    else()
        target_compile_definitions(${target} PUBLIC PH_DEV_TOOLS=0)
    endif()
endfunction()

# Fast, but still debuggable, Debug builds of hot code: the ECS and the math are templates the game instantiates,
# so a Debug game would otherwise crawl. Opt in per target with ph_optimize_debug(<target>).
function(ph_optimize_debug target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Ob1>)
    else()
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-Og>)
    endif()
endfunction()
