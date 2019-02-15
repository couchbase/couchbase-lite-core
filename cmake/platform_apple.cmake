include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals)
    # Use CommonCrypto for things like hashing and random numbers
    add_definitions(-D_CRYPTO_CC)
    set(LITECORE_CRYPTO_LIB "-framework Security" CACHE INTERNAL "")
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
    set_target_properties(
        LiteCoreREST PROPERTIES LINK_FLAGS
        "-exported_symbols_list ${CMAKE_CURRENT_SOURCE_DIR}/c4REST.exp"
    )
endfunction()