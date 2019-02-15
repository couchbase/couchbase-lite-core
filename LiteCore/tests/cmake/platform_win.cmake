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
    target_include_directories(
        CppTests PRIVATE
        ${TOP}vendor/mbedtls/include
        ${TOP}MSVC
    )

    target_link_libraries(
        CppTests PRIVATE
        ws2_32
        zlibstatic
    )
endfunction()