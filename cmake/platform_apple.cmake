include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals)
    setup_globals_unix()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(
            -fsanitize=address 
            -fsanitize=undefined 
            -fsanitize=nullability
            -fno-sanitize=enum,return,float-divide-by-zero,function,vptr
        )

        set_property(
            DIRECTORY 
            PROPERTY LINK_OPTIONS
            ${LINK_OPTIONS}
            -fsanitize=address 
            -fsanitize=undefined 
            -fsanitize=nullability
            -fno-sanitize=enum,return,float-divide-by-zero,function,vptr
        )
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
        "-framework SystemConfiguration"
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
