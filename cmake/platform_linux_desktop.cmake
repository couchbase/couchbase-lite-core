include("${CMAKE_CURRENT_LIST_DIR}/platform_linux.cmake")

function(setup_globals)
    setup_globals_linux()

    # Enable relative RPATHs for installed bits
    set (CMAKE_INSTALL_RPATH "\$ORIGIN" PARENT_SCOPE)

    if(NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        if(NOT "${CMAKE_CXX_COMPILER_ID}" MATCHES "(Apple)?Clang")
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
            PATHS "${CMAKE_BINARY_DIR}/tlm/deps/libcxx.exploded"
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
    if(NOT LITECORE_DISABLE_ICU)
        set (_icu_libs)
        foreach (_lib icuuc icui18n icudata)
            unset (_iculib CACHE)
            find_library(_iculib ${_lib})
            if (NOT _iculib)
                message(FATAL_ERROR "${_lib} not found")
            endif()
            list(APPEND _icu_libs ${_iculib})
        endforeach()
        set (ICU_LIBS ${_icu_libs} CACHE STRING "ICU libraries" FORCE)
        message("Found ICU libs at ${ICU_LIBS}")

        find_path(LIBICU_INCLUDE unicode/ucol.h
            HINTS "${CMAKE_BINARY_DIR}/tlm/deps/icu4c.exploded"
            PATH_SUFFIXES include)
        if (NOT LIBICU_INCLUDE)
            message(FATAL_ERROR "libicu header files not found")
        endif()
        message("Using libicu header files in ${LIBICU_INCLUDE}")
        include_directories("${LIBICU_INCLUDE}")
        mark_as_advanced(ICU_LIBS LIBICU_INCLUDE)
    endif()

    find_library(ZLIB_LIB z)
    if (NOT ZLIB_LIB)
        message(FATAL_ERROR "libz not found")
    endif()
    message("Found libz at ${ZLIB_LIB}")
    find_path(ZLIB_INCLUDE NAMES zlib.h
        HINTS "${CMAKE_BINARY_DIR}/tlm/deps/zlib.exploded"
        PATH_SUFFIXES include)
    if (NOT ZLIB_INCLUDE)
        message(FATAL_ERROR "libz header files not found")
    endif()
    include_directories(${ZLIB_INCLUDE})
    message("Using libz header files in ${ZLIB_INCLUDE}")

    # libc++ is special - clang will introduce an implicit -lc++ when it is used.
    # That means we need to tell the linker the path to the directory containing
    # libc++.so rather than just linking the .so directly. This must be done
    # *before* the target declaration as it affects all subsequent targets.
    get_filename_component (LIBCXX_LIBDIR "${LIBCXX_LIB}" DIRECTORY)
    link_directories (${LIBCXX_LIBDIR})

    mark_as_advanced(
        LIBCXX_INCLUDE LIBCXX_LIB LIBCXXABI_LIB LIBCXX_LIBDIR
        ZLIB_LIB ZLIB_INCLUDE
    )

    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # Work around the archaic requirement that the linker needs
        # libraries in a certain order
        link_libraries("-Wl,--start-group")
    endif()
endfunction()

function(set_litecore_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" ${oneValueArgs} "" ${ARGN})
     if(NOT DEFINED LINUX_SSS_RESULT)
         message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
     endif()

     set_litecore_source_linux(RESULT BASE_LITECORE_FILES)
     set(
         ${LINUX_SSS_RESULT}
         ${BASE_LITECORE_FILES}
         PARENT_SCOPE
     )
 endfunction()

function(set_support_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED LINUX_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_support_source_linux(RESULT LINUX_LITECORE_FILES)
    set(
        ${LINUX_SSS_RESULT}
        ${LINUX_LITECORE_FILES}
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_linux()

    target_link_libraries(
        LiteCore PRIVATE 
        ${ZLIB_LIB}
        ${ICU_LIBS}
    )
endfunction()

function(setup_support_build)
    setup_support_build_linux()
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
