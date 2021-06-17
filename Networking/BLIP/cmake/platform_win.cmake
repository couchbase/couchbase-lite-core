include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(WIN_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED WIN_SSS_RESULT)
        message(FATAL_ERROR "set_source_files needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${WIN_SSS_RESULT}
        ${BASE_SRC_FILES}
        ${SUPPORT_LOCATION}/ThreadedMailbox.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../vendor/zlib" "vendor/zlib")
    target_compile_definitions(
        BLIPStatic PRIVATE
        -DINCL_EXTRA_HTON_FUNCTIONS # Make sure htonll is defined for WebSocketProtocol.hh
        -DNOMINMAX                  # Windows, come on...stop it!
    )

    target_include_directories(
        BLIPStatic PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../vendor/zlib"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor/zlib"
        "${CMAKE_CURRENT_LIST_DIR}/../../MSVC"
    )

    target_link_libraries(
        BLIPStatic INTERFACE
       ${ZLIB_LIB}
    )
endfunction()