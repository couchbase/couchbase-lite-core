include("${CMAKE_CURRENT_LIST_DIR}/platform_linux.cmake")

MACRO (_install_gcc_file GCCFILENAME)
  IF (UNIX AND NOT APPLE)
    EXECUTE_PROCESS(
      COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX_FLAGS} -print-file-name=${GCCFILENAME}
      OUTPUT_VARIABLE _gccfile OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_VARIABLE _errormsg
      RESULT_VARIABLE _failure)
    IF (_failure)
      MESSAGE (FATAL_ERROR "Error (${_failure}) determining path to ${GCCFILENAME}: ${_errormsg}")
    ENDIF ()
    # We actually need to copy any files with longer filenames - this can be eg.
    # libstdc++.so.6, or libgcc_s.so.1.
    # Note: RPM demands that .so files be executable or else it won't
    # extract debug info from them.
    FILE (GLOB _gccfiles "${_gccfile}*")
    FOREACH (_gccfile ${_gccfiles})
      # Weird extraneous file not desired
      IF (_gccfile MATCHES ".py$")
        CONTINUE ()
      ENDIF ()
      INSTALL (FILES "${_gccfile}" DESTINATION lib
               PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                  GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    ENDFOREACH ()
  ENDIF ()
ENDMACRO (_install_gcc_file)

_install_gcc_file(libstdc++.so.6)
_install_gcc_file(libgcc_s.so.1)

IF (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    SET(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE STRING
        "The install location" FORCE)
    LIST(APPEND CMAKE_PREFIX_PATH "${CMAKE_INSTALL_PREFIX}")
ENDIF (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)

INCLUDE (${CMAKE_CURRENT_LIST_DIR}/../jenkins/CBDeps.cmake)

if(NOT LITECORE_DISABLE_ICU AND NOT LITECORE_DYNAMIC_ICU)
    # Install cbdeps packages using cbdep tool
    CBDEP_INSTALL (PACKAGE icu4c VERSION 76.1-1)
    FILE (COPY "${CBDEP_icu4c_DIR}/lib" DESTINATION "${CMAKE_INSTALL_PREFIX}")
endif()

function(setup_globals)
    setup_globals_linux()

    # Enable relative RPATHs for installed bits
    set (CMAKE_INSTALL_RPATH "\$ORIGIN" PARENT_SCOPE)
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
         LiteCore/Unix/icu_shim.c
         PARENT_SCOPE
     )
 endfunction()

function(setup_litecore_build)
    setup_litecore_build_linux()

    # zlib
    find_library(ZLIB_LIB z)
    if (NOT ZLIB_LIB)
        message(FATAL_ERROR "libz not found")
    endif()
    message("Found libz at ${ZLIB_LIB}")
    find_path(ZLIB_INCLUDE NAMES zlib.h PATH_SUFFIXES include)
    if (NOT ZLIB_INCLUDE)
        message(FATAL_ERROR "libz header files not found")
    endif()
    message("Using libz header files in ${ZLIB_INCLUDE}")

    mark_as_advanced(
           ZLIB_LIB ZLIB_INCLUDE
    )

    foreach(target ${LITECORE_TARGETS})
        # Suppress an annoying note about GCC 7 ABI changes, and linker errors about the Fleece C API
        target_compile_options(
            ${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi;-Wno-odr>"
        )

        if(${target} STREQUAL "LiteCore" OR
           ${target} STREQUAL "CouchbaseSqlite3" OR
           ${target} STREQUAL "LiteCoreWebSocket")
           target_include_directories(${target} PRIVATE ${ZLIB_INCLUDE})
        endif()
    endforeach()

    foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
        target_link_libraries(
           ${liteCoreVariant} INTERFACE
           Threads::Threads
        )
        target_include_directories(${liteCoreVariant} PRIVATE ${ZLIB_INCLUDE})
    endforeach()

    if(NOT LITECORE_DISABLE_ICU AND NOT LITECORE_DYNAMIC_ICU)
        foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
            target_link_libraries(
                ${liteCoreVariant} PUBLIC
                icu::icu4c
            )
       endforeach()
    endif()
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
