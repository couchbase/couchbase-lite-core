# Sets the minimum version of CMake required to build the native
# library. You should either keep the default value or only pass a
# value of 3.4.0 or lower.

cmake_minimum_required(VERSION 3.4.1)

project (C4Tests)

set(TOP ${CMAKE_SOURCE_DIR}/../../../../../..)
set(VENDOR_DIR ${TOP}/vendor)

set(LITECORE_DIR ${CMAKE_SOURCE_DIR}/../../../../litecore)
set(LITECORE_INTERMEDIATESBUILD_DIR ${LITECORE_DIR}/build/intermediates/cmake/release/obj)
#set(LITECORE_EXTERNALBUILD_DIR      ${LITECORE_DIR}/.externalNativeBuild/cmake/release)


add_definitions(-DC4_TESTS)

include_directories(${TOP}/C/include/
                    ${TOP}/C/
                    ${TOP}/C/tests
                    ${TOP}/LiteCore/Storage/
                    ${TOP}/LiteCore/Support/
                    ${TOP}/vendor/fleece/Fleece/
                    ${TOP}/vendor/fleece/vendor/catch/
                    ${TOP}/vendor/SQLiteCpp/sqlite3/
                )

# sqlite3
add_library(sqlite3 SHARED IMPORTED)
set_target_properties(sqlite3 PROPERTIES IMPORTED_LOCATION
    ${LITECORE_INTERMEDIATESBUILD_DIR}/${ANDROID_ABI}/libsqlite3.so)

# litecore
add_library(LiteCore SHARED IMPORTED)
set_target_properties(LiteCore PROPERTIES IMPORTED_LOCATION
    ${LITECORE_INTERMEDIATESBUILD_DIR}/${ANDROID_ABI}/libLiteCore.so)

# CppTests
set(LITECORE_C4TESTS_DIR ${TOP}/C/tests)

add_library( native-lib SHARED native-lib.cpp
    ${LITECORE_C4TESTS_DIR}/c4AllDocsPerformanceTest.cc
    ${LITECORE_C4TESTS_DIR}/c4BlobStoreTest.cc
    ${LITECORE_C4TESTS_DIR}/c4DatabaseTest.cc
    ${LITECORE_C4TESTS_DIR}/c4DocumentTest.cc
    ${LITECORE_C4TESTS_DIR}/c4KeyTest.cc
    ${LITECORE_C4TESTS_DIR}/c4ObserverTest.cc
    ${LITECORE_C4TESTS_DIR}/c4PerfTest.cc
    ${LITECORE_C4TESTS_DIR}/c4Test.cc
    ${LITECORE_C4TESTS_DIR}/c4ThreadingTest.cc
    ${LITECORE_C4TESTS_DIR}/c4ViewTest.cc
)

find_library( log-lib log )

target_link_libraries(  native-lib
                        LiteCore
                        sqlite3
                        ${log-lib} )
