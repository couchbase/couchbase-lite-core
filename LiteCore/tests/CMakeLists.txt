cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ../../)

add_definitions(-DDEBUG -DLITECORE_CPP_TESTS=1)

include_directories(${TOP}LiteCore/BlobStore/ ${TOP}LiteCore/Indexes/ ${TOP}LiteCore/RevTrees/
                    ${TOP}LiteCore/Storage/ ${TOP}LiteCore/Support/ ${TOP}LiteCore/VersionVectors/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/  # for catch.hpp
                    ${TOP}vendor/forestdb/include/libforestdb/
                    ${TOP}vendor/SQLiteCpp/include/
                )
                    
aux_source_directory(.  TEST_SRC)

add_executable(CppTests ${TEST_SRC})

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                forestdb
                                sqlite3
                                SQLite3_UnicodeSN)
