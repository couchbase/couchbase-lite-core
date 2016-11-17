cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DLITECORE_CPP_TESTS=1)
if(NOT MSVC)
    add_definitions(-DDEBUG)
endif()

include_directories(${TOP}LiteCore/BlobStore/ ${TOP}LiteCore/Indexes/ ${TOP}LiteCore/RevTrees/
                    ${TOP}LiteCore/Storage/ ${TOP}LiteCore/Support/ ${TOP}LiteCore/VersionVectors/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                    ${TOP}vendor/forestdb/include/libforestdb/
                    ${TOP}vendor/SQLiteCpp/include/
                )
                    
aux_source_directory(.  TEST_SRC)
if(WIN32)
	set(TEST_SRC ${TEST_SRC} ../../MSVC/arc4random.cc)
endif()

add_executable(CppTests ${TEST_SRC})

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                forestdb
                                sqlite3
                                SQLite3_UnicodeSN)
if(NOT APPLE)
    if(UNIX)
        target_link_libraries(CppTests  crypto  bsd)
    else()
        target_link_libraries(CppTests ${OPENSSL_CRYPTO_LIBRARY} ws2_32 )
        if(MSVC)
            add_custom_command(TARGET CppTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${TOP}build_cmake/vendor/forestdb/\$\(Configuration\)/forestdb.dll
                ${PROJECT_BINARY_DIR}/\$\(Configuration\))
            add_custom_command(TARGET CppTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${TOP}build_cmake/vendor/SQLiteCpp/sqlite3/\$\(Configuration\)/sqlite3.dll
                ${PROJECT_BINARY_DIR}/\$\(Configuration\))
            if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo)
                add_custom_command(TARGET CppTests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${TOP}build_cmake/vendor/forestdb/\$\(Configuration\)/forestdb.pdb
                    ${PROJECT_BINARY_DIR}/\$\(Configuration\))
                add_custom_command(TARGET CppTests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${TOP}build_cmake/vendor/SQLiteCpp/sqlite3/\$\(Configuration\)/sqlite3.pdb
                    ${PROJECT_BINARY_DIR}/\$\(Configuration\))
            endif()
        endif()
    endif()
endif()

