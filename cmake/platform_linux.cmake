include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")
include(CheckSymbolExists)
include(CheckLibraryExists)

macro(check_threading)
    check_threading_unix()

    if(CMAKE_HAVE_LIBC_CREATE OR CMAKE_HAVE_LIBC_PTHREAD)
        check_symbol_exists(pthread_getname_np pthread.h HAVE_PTHREAD_GETNAME_NP)
        check_symbol_exists(pthread_threadid_np pthread.h HAVE_PTHREAD_THREADID_NP)
    else()
        check_library_exists(pthread pthread_getname_np "" HAVE_PTHREAD_GETNAME_NP)
        check_library_exists(pthread pthread_threadid_np "" HAVE_PTHREAD_THREADID_NP)
    endif()

    check_include_file("sys/prctl.h" HAVE_PRCTL_H)
    if(HAVE_PRCTL_H)
        check_symbol_exists(prctl "sys/prctl.h" HAVE_PRCTL)
    endif()

    check_include_file("sys/syscall.h" HAVE_SYSCALL_H)
    if(HAVE_SYSCALL_H)
        check_symbol_exists(SYS_gettid "sys/syscall.h" HAVE_SYS_GETTID)
    endif()

    check_include_file("linux/unistd.h" HAVE_UNISTD_H)
    if(HAVE_UNISTD_H)
        check_symbol_exists(__NR_gettid "linux/unistd.h" HAVE_NR_GETTID)
    endif()
endmacro()

function(setup_globals_linux)
    setup_globals_unix()

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
        LiteCore/Unix/strlcat.c
        LiteCore/Support/StringUtil_icu.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build_linux)
    setup_litecore_build_unix()

    if(LITECORE_DYNAMIC_ICU)
        foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
            target_compile_definitions(
                ${liteCoreVariant} PRIVATE
                -DCBL_USE_ICU_SHIM
                -DLITECORE_USES_ICU=1
            )
        endforeach()
    elseif(NOT LITECORE_DISABLE_ICU)
        foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
            target_compile_definitions(
                ${liteCoreVariant} PRIVATE
                -DLITECORE_USES_ICU=1
            )
       endforeach()
    endif()

    foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
        target_include_directories(
            ${liteCoreVariant} PRIVATE
            LiteCore/Unix
        )
    endforeach()

    foreach(platform LiteCoreObjects LiteCoreUnitTesting BLIPObjects)
        target_compile_options(
            ${platform} PRIVATE
            #"-Wformat=2"
            "-Wno-format"
        )
    endforeach()
endfunction()
