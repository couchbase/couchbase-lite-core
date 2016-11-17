cmake_minimum_required (VERSION 2.6)
project (C4Tests)

set(TOP ../../)

add_definitions(-DDEBUG -DC4_TESTS)

include_directories(${TOP}C/include/
                    ${TOP}C/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                )
                    
aux_source_directory(.  TEST_SRC)
set(TEST_SRC ${TEST_SRC} ${TOP}/LiteCore/tests/main.cpp ${TOP}/LiteCore/Support/Logging.cc)
if(MSVC)
	set(TEST_SRC ${TEST_SRC} ${TOP}/MSVC/vasprintf-msvc.c)
endif()

add_executable(C4Tests ${TEST_SRC} )

target_link_libraries(C4Tests  LiteCore)
if(MSVC)
    add_custom_command(TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${TOP}build_cmake/vendor/forestdb/\$\(Configuration\)/forestdb.dll
        ${PROJECT_BINARY_DIR}/\$\(Configuration\))
    
    add_custom_command(TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${TOP}build_cmake/vendor/SQLiteCpp/sqlite3/\$\(Configuration\)/sqlite3.dll
        ${PROJECT_BINARY_DIR}/\$\(Configuration\))
    add_custom_command(TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${TOP}build_cmake/\$\(Configuration\)/LiteCore.dll
        ${PROJECT_BINARY_DIR}/\$\(Configuration\))
    if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo)
        add_custom_command(TARGET C4Tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TOP}build_cmake/vendor/forestdb/\$\(Configuration\)/forestdb.pdb
            ${PROJECT_BINARY_DIR}/\$\(Configuration\))
        add_custom_command(TARGET C4Tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TOP}build_cmake/vendor/SQLiteCpp/sqlite3/\$\(Configuration\)/sqlite3.pdb
            ${PROJECT_BINARY_DIR}/\$\(Configuration\))
        add_custom_command(TARGET C4Tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TOP}build_cmake/\$\(Configuration\)/LiteCore.pdb
            ${PROJECT_BINARY_DIR}/\$\(Configuration\))
    endif()
endif()

