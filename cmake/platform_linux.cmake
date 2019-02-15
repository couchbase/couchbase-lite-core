include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

function(setup_globals_linux)
    set(LITECORE_CRYPTO_LIB mbedcrypto CACHE INTERNAL "")
    add_definitions(-D_CRYPTO_MBEDTLS)

    set(WHOLE_LIBRARY_FLAG "-Wl,--whole-archive" CACHE INTERNAL "")
    set(NO_WHOLE_LIBRARY_FLAG "-Wl,--no-whole-archive" CACHE INTERNAL "")
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

    target_include_directories(
        LiteCoreStatic PRIVATE
        LiteCore/Unix
    )
endfunction()

function(setup_support_build)
    target_include_directories(
        Support PRIVATE
        LiteCore/Unix
    )
endfunction()