include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals)
    setup_globals_unix()

    if(NOT -Wno-unknown-pragmas IN_LIST LITECORESTATIC_FLAGS)
        message(WARNING "Disabling -Wunknown-pragma and -Wsign-compare")
        list(
            APPEND LINUX_FLAGS
            ${LITECORESTATIC_FLAGS}
            -Wnewline-eof
        )

        set(LITECORESTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(BLIPSTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(FLEECESTATIC_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(SUPPORT_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(FLEECEBASE_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
        set(LITECOREWEBSOCKET_FLAGS ${LINUX_FLAGS} CACHE INTERNAL "")
    endif()
endfunction()

function(set_litecore_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(APPLE_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED APPLE_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_base(RESULT BASE_LITECORE_FILES)
    set(
        ${APPLE_SSS_RESULT}
        ${BASE_LITECORE_FILES}
        LiteCore/Storage/UnicodeCollator_Apple.cc
        PARENT_SCOPE
    )
endfunction()

function(set_support_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(APPLE_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED APPLE_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_support_source_base(RESULT BASE_SUPPORT_FILES)
    set(
        ${APPLE_SSS_RESULT}
        ${BASE_SUPPORT_FILES}
        LiteCore/Support/StringUtil_Apple.mm
        LiteCore/Support/LibC++Debug.cc
        LiteCore/Support/Instrumentation.cc
        Crypto/PublicKey+Apple.mm
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_unix()

    target_link_libraries(
        LiteCore PUBLIC
        "-framework CoreFoundation"
        "-framework Foundation"
        z
    )

    # Specify list of symbols to export
    set_target_properties(
        LiteCore PROPERTIES LINK_FLAGS
        "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4.exp"
    )
endfunction()

function(setup_support_build)
    # No-op
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
