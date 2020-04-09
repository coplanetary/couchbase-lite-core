include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals_linux)
    setup_globals_unix()

    set(WHOLE_LIBRARY_FLAG "-Wl,--whole-archive" CACHE INTERNAL "")
    set(NO_WHOLE_LIBRARY_FLAG "-Wl,--no-whole-archive" CACHE INTERNAL "")
    
    if(NOT -Wno-unknown-pragmas IN_LIST LITECORESTATIC_FLAGS)
        message(WARNING "Disabling -Wunknown-pragma, -Wsign-compare, and -Wstrict-aliasing")
        list(
            APPEND LINUX_FLAGS
            ${LITECORESTATIC_FLAGS}
            -Wno-unknown-pragmas
            -Wno-sign-compare
            -Wno-strict-aliasing
        )

        set(LITECORESTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(BLIPSTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(FLEECESTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(SUPPORT_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(FLEECEBASE_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(LITECOREWEBSOCKET_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
    endif()
endfunction()

function(set_litecore_source_linux)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED LINUX_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_base(RESULT BASE_LITECORE_FILES)
    set(
        ${LINUX_SSS_RESULT}
        ${BASE_LITECORE_FILES}
        LiteCore/Storage/UnicodeCollator_ICU.cc
        PARENT_SCOPE
    )
endfunction()

function(set_support_source_linux)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED LINUX_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_support_source_base(RESULT BASE_SUPPORT_FILES)
    set(
        ${LINUX_SSS_RESULT}
        ${BASE_SUPPORT_FILES}
        LiteCore/Unix/strlcat.c
        LiteCore/Unix/arc4random.cc
        LiteCore/Support/StringUtil_icu.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build_linux)
    setup_litecore_build_unix()

    if(NOT LITECORE_DISABLE_ICU)
        target_compile_definitions(
            LiteCoreStatic PRIVATE
            -DLITECORE_USES_ICU=1
        )
    endif()

    target_include_directories(
        LiteCoreStatic PRIVATE
        LiteCore/Unix
    )

    # Specify list of symbols to export
    set_target_properties(
        LiteCore PROPERTIES LINK_FLAGS
        "-Wl,--version-script=${PROJECT_SOURCE_DIR}/C/c4.gnu"
    )
endfunction()

function(setup_support_build_linux)
    if(NOT LITECORE_DISABLE_ICU)
        target_compile_definitions(
            Support PRIVATE
            -DLITECORE_USES_ICU=1
        )
    endif()

    target_include_directories(
        Support PRIVATE
        LiteCore/Unix
    )
endfunction()
