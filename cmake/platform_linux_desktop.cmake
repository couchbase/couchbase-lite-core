include("${CMAKE_CURRENT_LIST_DIR}/platform_linux.cmake")

function(setup_globals)
    setup_globals_linux()

    # Enable relative RPATHs for installed bits
    set (CMAKE_INSTALL_RPATH "\$ORIGIN")

    if(NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        if(NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
            message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} is not supported for building!")
        endif()
        find_library(LIBCXX_LIB c++)
        if (NOT LIBCXX_LIB)
            message(FATAL_ERROR "libc++ not found")
        endif()
        message("Found libc++ at ${LIBCXX_LIB}")
        set(LITECORE_CXX_FLAGS "${LITECORE_CXX_FLAGS} -stdlib=libc++" CACHE INTERNAL "")

        find_library(LIBCXXABI_LIB c++abi)
        if (NOT LIBCXXABI_LIB)
            message(FATAL_ERROR "libc++abi not found")
        endif()
        message("Found libc++abi at ${LIBCXXABI_LIB}")
        find_path(LIBCXX_INCLUDE c++/v1/string
            HINTS "${CMAKE_BINARY_DIR}/tlm/deps/libcxx.exploded"
            PATH_SUFFIXES include)
        if (NOT LIBCXX_INCLUDE)
            message(FATAL_ERROR "libc++ header files not found")
        endif()
        message("Using libc++ header files in ${LIBCXX_INCLUDE}")
        include_directories("${LIBCXX_INCLUDE}/c++/v1")
        if(NOT EXISTS "/usr/include/xlocale.h")
            include_directories("${LIBCXX_INCLUDE}/c++/v1/support/xlocale") # this fixed path is here to avoid compilation on Ubuntu 17.10 where xlocale.h is searched by some header(s) in libc++ as <xinclude.h> but not found from search path without this modification.  However, only do it if the original xlocale.h does not exist since this will get searched before /usr/include and override a valid file with an empty one.
        endif()
        include_directories("/usr/include/libcxxabi") # this fixed path is here to avoid Clang issue noted at http://lists.alioth.debian.org/pipermail/pkg-llvm-team/2015-September/005208.html
    endif()
    find_library(ICU4C_COMMON icuuc)
    if (NOT ICU4C_COMMON)
        message(FATAL_ERROR "libicuuc not found")
    endif()
    message("Found libicuuc at ${ICU4C_COMMON}")
    find_library(ICU4C_I18N icui18n)
    if (NOT ICU4C_I18N)
        message(FATAL_ERROR "libicui18n not found")
    endif()
    message("Found libicui18n at ${ICU4C_I18N}")
    find_path(LIBICU_INCLUDE unicode/ucol.h
        HINTS "${CMAKE_BINARY_DIR}/tlm/deps/icu4c.exploded"
        PATH_SUFFIXES include)
    if (NOT LIBICU_INCLUDE)
        message(FATAL_ERROR "libicu header files not found")
    endif()
    message("Using libicu header files in ${LIBICU_INCLUDE}")
    include_directories("${LIBICU_INCLUDE}")

    # libc++ is special - clang will introduce an implicit -lc++ when it is used.
    # That means we need to tell the linker the path to the directory containing
    # libc++.so rather than just linking the .so directly. This must be done
    # *before* the target declaration as it affects all subsequent targets.
    get_filename_component (LIBCXX_LIBDIR "${LIBCXX_LIB}" DIRECTORY)
    link_directories (${LIBCXX_LIBDIR})
endfunction()

function(set_support_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED LINUX_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_linux(RESULT LINUX_LITECORE_FILES)
    set(
        ${LINUX_SSS_RESULT}
        ${LINUX_LITECORE_FILES}
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_linux()
    
    if(NOT ${LITECORE_DISABLE_ICU})
        target_compile_definitions(
            LiteCoreStatic PRIVATE
            -DLITECORE_USES_ICU=1
        )
    endif()

    target_link_libraries(
        LiteCore PRIVATE 
        z 
        ${ICU4C_COMMON} 
        ${ICU4C_I18N}
    )
endfunction()

function(setup_rest_build)
    # No-op
endfunction()