macro(generate_edition)
    if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
        # Script mode, use passed values
        set(CBLITE_CE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
        if(NOT DEFINED VERSION)
            message(
                FATAL_ERROR 
                "No version information passed (use -DVERSION=X.Y.Z)"
            )
        endif()
        set(CBL_LITECORE_BUILDID ${VERSION})

        if(NOT DEFINED OUTPUT_DIR)
            message(
                FATAL_ERROR 
                "No output directory information passed (use -DOUTPUT_DIR=...)"
            )
        endif()

        if(NOT DEFINED BLD_NUM)
            message(
                FATAL_ERROR 
                "No build number information passed (use -DBLD_NUM=###)"
            )
        endif()
        set(CBL_LITECORE_BUILDNUM ${BLD_NUM})
        set(CBL_LITECORE_OFFICIAL true)
    else()
        # CMakeLists.txt mode, use already available values or environment
        set(CBLITE_CE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set(OUTPUT_DIR ${GENERATED_HEADERS_DIR})
        set(CBL_LITECORE_OFFICIAL true)

        if(DEFINED ENV{VERSION})
            message(VERBOSE "Using VERSION:$ENV{VERSION} from environment variable")
            set(CBL_LITECORE_BUILDID $ENV{VERSION})
        else()
            message(WARNING "No VERSION set, defaulting to 0.0.0...")
            set(CBL_LITECORE_BUILDID "0.0.0")
            set(CBL_LITECORE_OFFICIAL false)
        endif()

        if(DEFINED ENV{BLD_NUM})
            message(VERBOSE "Using BLD_NUM:$ENV{BLD_NUM} from environment variable")
            set(CBL_LITECORE_BUILDNUM $ENV{BLD_NUM})
        else()
            message(WARNING "No BLD_NUM set...")
            set(CBL_LITECORE_OFFICIAL false)
        endif()
    endif()

    if(DEFINED ENV{LITECORE_VERSION_STRING})
        set(CBL_LITECORE_VERSION $ENV{LITECORE_VERSION_STRING})
    else()
        set(CBL_LITECORE_VERSION ${CBL_LITECORE_BUILDID})
    endif()

    find_package(Git)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY ${CBLITE_CE_DIR} 
            OUTPUT_VARIABLE CBL_GIT_COMMIT
            RESULT_VARIABLE SUCCESS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(NOT SUCCESS EQUAL 0)
            message(WARNING "Failed to get CE hash of build!")
        endif()
        
        if(BUILD_ENTERPRISE)
            set(COUCHBASE_ENTERPRISE TRUE) # This will be unset in script mode
            set(EE_PATH ${CBLITE_CE_DIR}/../couchbase-lite-core-EE)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                WORKING_DIRECTORY ${EE_PATH} 
                OUTPUT_VARIABLE CBL_GIT_COMMIT_EE
                RESULT_VARIABLE SUCCESS
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            if(NOT SUCCESS EQUAL 0)
                message(WARNING "Failed to get EE hash of build!")
            endif()
        endif()

        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY ${CBLITE_CE_DIR} 
            OUTPUT_VARIABLE BRANCH
            RESULT_VARIABLE SUCCESS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(NOT SUCCESS EQUAL 0)
            message(WARNING "Failed to get branch of build!")
            set(CBL_GIT_BRANCH "<unknown branch>")
        else()
            set(CBL_GIT_BRANCH ${BRANCH})
        endif()

        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain
            WORKING_DIRECTORY ${CBLITE_CE_DIR} 
            OUTPUT_VARIABLE CHANGES
            RESULT_VARIABLE SUCCESS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(SUCCESS EQUAL 0)
            if(NOT CHANGES STREQUAL "")
                set(CBL_GIT_DIRTY "+CHANGES")
            endif()
        endif()


    else()
        set(CBL_GIT_COMMIT "<unknown commit>")
        set(CBL_GIT_BRANCH "<unknown branch>")
    endif()
    
    configure_file(
        "${CBLITE_CE_DIR}/cmake/repo_version.h.in"
        "${OUTPUT_DIR}/repo_version.h"
    )
    message(STATUS "Wrote ${OUTPUT_DIR}/repo_version.h...")
endmacro()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    generate_edition()
endif()