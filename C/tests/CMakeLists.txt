#[[
LiteCore Shared Library Tests CMake Project

This project builds a test runner that links against the shared LiteCore library to
verify that the exported C API is working correctly.  It uses some predefined data
as follows:

- Everything in the 'data' directory
- iosdb.cblite2 database from ../../LiteCore/tests/data/ios120

The test runner depends on multiple other libraries from multiple git repositories:

- LiteCore shared library (../..)
- LiteCore REST shared library (../../REST)
- FleeceBase static library (../../vendor/fleece)
- Support static library (../../Support [partial])
- BLIP static library (../../vendor/BLIP-Cpp)

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

set_source_files(RESULT TEST_SRC)
add_executable(C4Tests ${TEST_SRC})
setup_build()

target_compile_definitions(
    C4Tests PRIVATE
    -DC4_TESTS
    -DCATCH_CONFIG_CPP11_STREAM_INSERTABLE_CHECK
)

target_link_libraries(
    C4Tests PRIVATE
    LiteCore
    LiteCoreREST_Static
    LiteCoreWebSocket
    Support
    BLIPStatic
    FleeceBase
    mbedcrypto
    mbedtls
)

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
    ${TOP}Networking
    ${TOP}LiteCore/Support
    ${TOP}vendor/SQLiteCpp/sqlite3
    ${TOP}vendor/mbedtls/include
)

# Copy data files to output directory
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(COPY ../../LiteCore/tests/data/replacedb/ios120/iosdb.cblite2
  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests/data/replacedb/ios120)
