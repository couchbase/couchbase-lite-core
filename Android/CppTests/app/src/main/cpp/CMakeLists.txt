# Sets the minimum version of CMake required to build the native
# library. You should either keep the default value or only pass a
# value of 3.4.0 or lower.

cmake_minimum_required(VERSION 3.4.1)

set(TOP ${CMAKE_SOURCE_DIR}/../../../../../..)
set(VENDOR_DIR ${TOP}/vendor)

add_definitions(-DLITECORE_CPP_TESTS=1 -D_USE_MATH_DEFINES)

include_directories(${TOP}/LiteCore/Android/
                    ${TOP}/LiteCore/BlobStore/
                    ${TOP}/LiteCore/Database/
                    ${TOP}/LiteCore/Indexes/
                    ${TOP}/LiteCore/Query/
                    ${TOP}/LiteCore/RevTrees/
                    ${TOP}/LiteCore/Storage/
                    ${TOP}/LiteCore/Support/
                    ${TOP}/LiteCore/VersionVectors/
                    ${TOP}/vendor/fleece/Fleece/
                    ${TOP}/vendor/fleece/vendor/catch/
                    ${TOP}/vendor/SQLiteCpp/include/
                )

# configure import libs
set(OPENSSL_LIB_DIR ${TOP}/vendor/openssl/libs/android/clang)
set(LITECORE_DIR ${CMAKE_SOURCE_DIR}/../../../../litecore)
set(LITECORE_INTERMEDIATESBUILD_DIR ${LITECORE_DIR}/build/intermediates/cmake/release/obj)
set(LITECORE_EXTERNALBUILD_DIR      ${LITECORE_DIR}/.externalNativeBuild/cmake/release)

# sqlite3
add_library(sqlite3 SHARED IMPORTED)
set_target_properties(sqlite3 PROPERTIES IMPORTED_LOCATION
    ${LITECORE_INTERMEDIATESBUILD_DIR}/${ANDROID_ABI}/libsqlite3.so)

# litecore
add_library(LiteCoreStatic STATIC IMPORTED)
set_target_properties(LiteCoreStatic PROPERTIES IMPORTED_LOCATION
    ${LITECORE_EXTERNALBUILD_DIR}/${ANDROID_ABI}/libLiteCoreStatic.a)

# FleeceStatic
add_library(FleeceStatic STATIC IMPORTED)
set_target_properties(FleeceStatic PROPERTIES IMPORTED_LOCATION
    ${LITECORE_EXTERNALBUILD_DIR}/${ANDROID_ABI}/vendor/fleece/libFleeceStatic.a)

# SQLite3_UnicodeSN
add_library(SQLite3_UnicodeSN STATIC IMPORTED)
set_target_properties(SQLite3_UnicodeSN PROPERTIES IMPORTED_LOCATION
   ${LITECORE_EXTERNALBUILD_DIR}/${ANDROID_ABI}/vendor/sqlite3-unicodesn/libSQLite3_UnicodeSN.a)

# crypto
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION
        ${OPENSSL_LIB_DIR}/${ANDROID_ABI}/libcrypto.a)

# CppTests
set(LITECORE_CPPTESTS_DIR ${TOP}/LiteCore/tests)

add_library( native-lib SHARED native-lib.cpp
    ${LITECORE_CPPTESTS_DIR}/CASRevisionStoreTest.cc
    ${LITECORE_CPPTESTS_DIR}/CollatableTest.cc
    ${LITECORE_CPPTESTS_DIR}/DataFileTest.cc
    ${LITECORE_CPPTESTS_DIR}/DocumentKeysTest.cc
    ${LITECORE_CPPTESTS_DIR}/IndexTest.cc
    ${LITECORE_CPPTESTS_DIR}/LiteCoreTest.cc
    ${LITECORE_CPPTESTS_DIR}/MapReduceTest.cc
    ${LITECORE_CPPTESTS_DIR}/QueryParserTest.cc
    ${LITECORE_CPPTESTS_DIR}/RevisionStoreTest.cc
    ${LITECORE_CPPTESTS_DIR}/RevisionTest.cc
    ${LITECORE_CPPTESTS_DIR}/SequenceTrackerTest.cc
    ${LITECORE_CPPTESTS_DIR}/SQLiteFunctionsTest.cc
    ${LITECORE_CPPTESTS_DIR}/VersionedDocumentTests.cc
    ${LITECORE_CPPTESTS_DIR}/VersionVectorTest.cc
    ${TOP}/LiteCore/Android/arc4random.cc
    )

find_library( log-lib log )

target_include_directories(native-lib PRIVATE ${DISTRIBUTION_DIR}/sqlite3/include ${distribution_DIR}/cpptests/include)

target_link_libraries(  native-lib
                        LiteCoreStatic
                        FleeceStatic
                        SQLite3_UnicodeSN
                        crypto
                        sqlite3
                        ${log-lib})