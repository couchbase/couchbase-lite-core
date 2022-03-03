include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(EMSCRIPTEN_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED EMSCRIPTEN_SSS_RESULT)
        message(FATAL_ERROR "set_source_files needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${EMSCRIPTEN_SSS_RESULT}
        ${BASE_SRC_FILES}
        ${SUPPORT_LOCATION}/ThreadedMailbox.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../vendor/zlib" "vendor/zlib")
    target_include_directories(
        BLIPStatic PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../vendor/zlib"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/zlib"
        ${LITECORE_LOCATION}/LiteCore/Unix
    )

    target_link_libraries(
        BLIPStatic INTERFACE
        zlibstatic
    )
endfunction()
