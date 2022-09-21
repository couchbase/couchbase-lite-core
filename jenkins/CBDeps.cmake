IF (NOT CBDeps_INCLUDED)
  SET (CBDeps_INCLUDED 1)

  # Global variables
  SET (CBDEP_VERSION 1.1.2)

  INCLUDE ("${CMAKE_CURRENT_LIST_DIR}/ParseArguments.cmake")

  # Downloads a file from a URL to a local file, raising any errors.
  FUNCTION (_DOWNLOAD_FILE url file)
    FILE (DOWNLOAD "${url}" "${file}.temp" STATUS _stat SHOW_PROGRESS)
    LIST (GET _stat 0 _retval)
    IF (_retval)
      FILE (REMOVE "${file}.temp")
      LIST (GET _stat 0 _errcode)
      LIST (GET _stat 1 _message)
      MESSAGE (FATAL_ERROR "Error downloading ${url}: ${_message} (${_errcode})")
    ENDIF (_retval)
    FILE (RENAME "${file}.temp" "${file}")
  ENDFUNCTION (_DOWNLOAD_FILE)

  # Generic function for installing a cbdep (2.0) package to a given directory
  # Required arguments:
  #   PACKAGE - package to install
  #   VERSION - version number of package (must be understood by 'cbdep' tool)
  # Optional arguments:
  #   INSTALL_DIR - where to install to; defaults to CMAKE_BINARY_DIR/cbdeps
  FUNCTION (CBDEP_INSTALL)
    # Download cbdep tool if necessary
    STRING (TOLOWER ${CMAKE_SYSTEM_PROCESSOR} _arch)
    SET (CBDEP_FILENAME "cbdep-${CBDEP_VERSION}-linux-${_arch}")
    SET (CBDEP_EXE "${CMAKE_BINARY_DIR}/${CBDEP_FILENAME}")
    IF (NOT EXISTS "${CBDEP_EXE}")
      MESSAGE (STATUS "Downloading cbdep ${CBDEP_VERSION}")
      _DOWNLOAD_FILE (
        "https://packages.couchbase.com/cbdep/${CBDEP_VERSION}/${CBDEP_FILENAME}"
        "${CBDEP_EXE}.temp"
      )
      FILE (CHMOD "${CBDEP_EXE}.temp" FILE_PERMISSIONS OWNER_EXECUTE OWNER_READ OWNER_WRITE)
      FILE (RENAME "${CBDEP_EXE}.temp" "${CBDEP_EXE}")
    ENDIF ()
    MESSAGE (STATUS "Using cbdep at ${CBDEP_EXE}")

    PARSE_ARGUMENTS (cbdep "" "INSTALL_DIR;PACKAGE;VERSION" "" ${ARGN})
    IF (NOT cbdep_INSTALL_DIR)
      SET (cbdep_INSTALL_DIR "${CMAKE_BINARY_DIR}/cbdeps")
    ENDIF ()
    IF ("${cbdep_PACKAGE}" STREQUAL "golang")
      SET (cbdep_TARGET_DIR "${cbdep_INSTALL_DIR}/go${cbdep_VERSION}")
    ELSE ()
      SET (cbdep_TARGET_DIR "${cbdep_INSTALL_DIR}/${cbdep_PACKAGE}-${cbdep_VERSION}")
    ENDIF ()
    IF (NOT IS_DIRECTORY "${cbdep_TARGET_DIR}")
      MESSAGE (STATUS "Downloading and caching ${cbdep_PACKAGE}-${cbdep_VERSION}")
      # Will need to rethink the "-p linux" part if we need cbdeps on other
      # platforms in future
      EXECUTE_PROCESS (
        COMMAND "${CBDEP_EXE}"
          -p linux
          install -d "${cbdep_INSTALL_DIR}"
          ${cbdep_PACKAGE} ${cbdep_VERSION}
        RESULT_VARIABLE _cbdep_result
        OUTPUT_VARIABLE _cbdep_out
        ERROR_VARIABLE _cbdep_out
      )
      IF (_cbdep_result)
        FILE (REMOVE_RECURSE "${cbdep_INSTALL_DIR}")
        MESSAGE (FATAL_ERROR "Failed installing cbdep ${cbdep_PACKAGE} ${cbdep_VERSION}: ${_cbdep_out}")
      ENDIF ()
    ENDIF ()
    SET (CBDEP_${cbdep_PACKAGE}_VERSION "${cbdep_VERSION}"
         CACHE STRING "Version of cbdep package '${cbdep_PACKAGE}'" FORCE)
    SET (CBDEP_${cbdep_PACKAGE}_DIR "${cbdep_TARGET_DIR}"
         CACHE STRING "Install location of cbdep package '${cbdep_PACKAGE}'" FORCE)
    MESSAGE (STATUS "Using cbdeps package ${cbdep_PACKAGE} ${cbdep_VERSION}")
  ENDFUNCTION (CBDEP_INSTALL)

ENDIF (NOT CBDeps_INCLUDED)
