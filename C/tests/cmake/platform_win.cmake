include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(WIN_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED WIN_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${WIN_SSS_RESULT}
        ${BASE_SRC_FILES}
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/LiteCore)

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}MSVC
    )

    target_include_directories(
        LiteCoreWebSocket PRIVATE
        ${TOP}MSVC
    )

    target_link_libraries(
        C4Tests PRIVATE
        mbedcrypto
        FleeceBase
        Support
        BLIPStatic
    )
    
    add_custom_command(
        TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DFilesToCopy="${FilesToCopy}"
        -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
        -P ${TOP}MSVC/copy_artifacts.cmake
    )
endfunction()