include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")
include(CheckSymbolExists)

macro(check_threading)
    check_threading_unix()
    
    check_symbol_exists(pthread_getname_np pthread.h HAVE_PTHREAD_GETNAME_NP)
    check_symbol_exists(pthread_threadid_np pthread.h HAVE_PTHREAD_THREADID_NP)
endmacro()

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
        LiteCore/Support/Instrumentation.cc
        Crypto/PublicKey+Apple.mm
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_unix()

    foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
        target_compile_definitions(
            ${liteCoreVariant} PUBLIC
            -DPERSISTENT_PRIVATE_KEY_AVAILABLE
        )
        target_link_libraries(
            ${liteCoreVariant} INTERFACE
            "-framework Security"
            "-framework SystemConfiguration"
        )
    endforeach()

    foreach(platform LiteCoreObjects LiteCoreUnitTesting BLIPObjects)
        target_compile_options(
            ${platform} PRIVATE
            "-Wformat"
            "-Wformat-nonliteral"
            "-Wformat-security"
            "-fobjc-arc"
        )
    endforeach()
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
