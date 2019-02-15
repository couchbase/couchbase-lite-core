include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(APPLE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED APPLE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${APPLE_SSS_RESULT}
        ${BASE_SRC_FILES}
        ${TOP}vendor/fleece/Tests/ObjCTests.mm
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    target_link_libraries(
        CppTests PRIVATE
        "-framework Foundation"
        "-framework CFNetwork"
        z
    )
endfunction()