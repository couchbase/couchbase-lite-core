#[[
LiteCore Shared Library Tests CMake Project

This project builds a test runner that links against the shared LiteCore library to
verify that the exported C API is working correctly.  It uses some predefined data
as follows:

- Everything in the 'data' directory
- iosdb.cblite2 database from ../../LiteCore/tests/data/ios120

The test runner depends on multiple other libraries from multiple git repositories:

- LiteCore shared library (../..)

In addition, there are also some system dependencies that are not listed here
]]#
cmake_minimum_required (VERSION 3.9)
project (C4Tests)

if(ANDROID OR WINDOWS_STORE)
    # No sense building it for Android since using an Android executable is
    # tricky and strange.  Windows Store projects are also not supported here.
    return()
endif()

if(NOT LITECORE_BUILD_TESTS)
    return()
endif()

# This project is not standalone.  Point to the "root" directory
set(TOP ${PROJECT_SOURCE_DIR}/../../)

if(MSVC)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_win.cmake")
elseif(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_apple.cmake")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_linux.cmake")
else()
    message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

add_executable(
    C4Tests
    c4AllDocsPerformanceTest.cc
    c4BlobStoreTest.cc
    c4DatabaseInternalTest.cc
    c4DatabaseTest.cc
    c4DocumentTest.cc
    c4ObserverTest.cc
    c4PerfTest.cc
    c4QueryTest.cc
    c4Test.cc
    c4ThreadingTest.cc
    # EE tests:
    c4DatabaseEncryptionTest.cc
    c4CertificateTest.cc
    
    ${TOP}LiteCore/tests/main.cpp
    ${TOP}Crypto/SecureRandomize.cc
    ${TOP}LiteCore/Support/FilePath.cc
    ${TOP}LiteCore/Support/LogDecoder.cc
    ${TOP}LiteCore/Support/Logging_Stub.cc
    ${TOP}LiteCore/Support/StringUtil.cc
    ${TOP}LiteCore/Support/TestsCommon.cc
    ${TOP}LiteCore/Support/Error.cc
    ${TOP}vendor/fleece/ObjC/slice+CoreFoundation.cc
    ${TOP}vendor/fleece/vendor/libb64/cencode.c
    ${TOP}vendor/fleece/vendor/libb64/cdecode.c
)
setup_build()

target_compile_definitions(
    C4Tests PRIVATE
    -DC4_TESTS
    -DCATCH_CONFIG_CPP11_STREAM_INSERTABLE_CHECK
)

if(NOT LITECORE_PREBUILT_LIB STREQUAL "")
    message(STATUS "Skipping LiteCore build, using ${LITECORE_PREBUILT_LIB}")
    target_link_libraries(
        C4Tests PRIVATE
        ${LITECORE_PREBUILT_LIB}
        FleeceBase
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}C/include
    )
else()
    target_link_libraries(
        C4Tests PRIVATE
        LiteCore
        FleeceBase
    )
endif()

target_include_directories(
    C4Tests PRIVATE
    .
    ${TOP}vendor/fleece/vendor/catch
    ${TOP}vendor/fleece/API
    ${TOP}vendor/fleece/Fleece/Support
    ${TOP}C
    ${TOP}Crypto
    ${TOP}Replicator
    ${TOP}Replicator/tests
    ${TOP}REST
    ${TOP}REST/tests
    ${TOP}Networking
    ${TOP}Networking/WebSockets
    ${TOP}LiteCore/Support
    ${TOP}vendor/SQLiteCpp/sqlite3
    ${TOP}vendor/SQLiteCpp/include
    ${TOP}vendor/mbedtls/crypto/include
    ${TOP}vendor/fleece/vendor/date/include
    ${TOP}vendor/fleece/vendor/libb64
)

# Copy data files to output directory
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(COPY ../../LiteCore/tests/data/replacedb/ios120/iosdb.cblite2
  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests/data/replacedb/ios120)
