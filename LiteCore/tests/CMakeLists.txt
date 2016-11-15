cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ../../)

add_definitions(-DDEBUG -DLITECORE_CPP_TESTS=1)

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
        target_link_libraries(CppTests ${OPENSSL_LIBRARIES} ws2_32 )
    endif()
endif()

