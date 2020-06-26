include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals)
    setup_globals_unix()
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
        LiteCore/Support/StringUtil_Apple.mm
        LiteCore/Support/LibC++Debug.cc
        LiteCore/Support/Instrumentation.cc
        Crypto/PublicKey+Apple.mm
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_unix()

    target_compile_definitions(
        LiteCoreStatic PUBLIC
        -DPERSISTENT_PRIVATE_KEY_AVAILABLE
    )

    target_link_libraries(
        LiteCoreStatic INTERFACE
        "-framework Security"
    )

    target_link_libraries(
        LiteCoreWebSocket INTERFACE
        "-framework SystemConfiguration"
    )

    # Specify list of symbols to export
    if(BUILD_ENTERPRISE)
        set_target_properties(
            LiteCore PROPERTIES LINK_FLAGS
            "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4_ee.exp"
        )
    else()
        set_target_properties(
            LiteCore PROPERTIES LINK_FLAGS
            "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4.exp"
        )
    endif()
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
