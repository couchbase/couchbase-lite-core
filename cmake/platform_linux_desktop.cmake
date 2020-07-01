include("${CMAKE_CURRENT_LIST_DIR}/platform_linux.cmake")

function(setup_globals)
    setup_globals_linux()

    # Enable relative RPATHs for installed bits
    set (CMAKE_INSTALL_RPATH "\$ORIGIN" PARENT_SCOPE)

    # NOTE: We used to do a whole dog and pony show here to get clang to use libc++.  Now we don't care
    # and if the person using this project wants to do so then they will have to set the options
    # accordingly

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

    mark_as_advanced(
	ICU_LIBS LIBICU_INCLUDE ZLIB_LIB ZLIB_INCLUDE
    )
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

function(setup_litecore_build)
    setup_litecore_build_linux()

    target_link_libraries(
        BLIPStatic INTERFACE
	    pthread
    )

    target_link_libraries(
        LiteCore INTERFACE
	    pthread
    )
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()

    target_link_libraries(LiteCoreREST_Static INTERFACE mbedtls)
endfunction()
