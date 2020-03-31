#[[
LiteCore Static Library Tests CMake Project

This project builds a test runner that links against the static LiteCore library to
verify that the inner logic is working correctly (i.e. non exported functions, etc).
It uses some predefined data as follows:

- Everything in the 'data' directory
- Everything in the '../../C/tests/data' directory
- Fleece and JSON data files from '../../vendor/fleece/Tests'

The test runner depends on multiple other libraries from multiple git repositories:

- LiteCore static library (../..)
- Fleece static library (../../vendor/fleece)
- mbedcrypto static library (../../vendor/mbedtls)
- SQLite Unicode snowball static library (../../vendor/sqlite3_unicodesn)
- BLIP static library (../../vendor/BLIP-Cpp)
- CivetWeb static library (../../vendor/civetweb)
- Support static library (../../Support [partial])

In addition, there are also some system dependencies that are not listed here
]]#
cmake_minimum_required (VERSION 3.9)
project (CppTests)

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

# Copy data files to output directory
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests)
file(COPY ../../Replicator/tests/data/cert.pem DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/Replicator/tests/data/)
file(COPY ../../C/tests/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(GLOB FLEECE_FILES "../../vendor/fleece/Tests/*.json" "../../vendor/fleece/Tests/*.json5" "../../vendor/fleece/Tests/*.fleece" "../../vendor/fleece/Tests/*.txt")
file(COPY ${FLEECE_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/vendor/fleece/Tests)
add_executable(CppTests ${TEST_SRC})

setup_build()

target_compile_definitions(
    CppTests PRIVATE
    -DLITECORE_CPP_TESTS=1
    -D_USE_MATH_DEFINES     # Define math constants like PI
    -DNOMINMAX              # Get rid of min/max macros that interfere with std::min/std::max
    -DCATCH_CONFIG_CPP11_STREAM_INSERTABLE_CHECK 
)

target_include_directories(
    CppTests PRIVATE
    ${TOP}vendor/fleece/API
    ${TOP}vendor/fleece/Experimental
    ${TOP}vendor/fleece/Fleece/Core
    ${TOP}vendor/fleece/Fleece/Mutable
    ${TOP}vendor/fleece/Fleece/Support
    ${TOP}vendor/fleece/Fleece/Tree
    ${TOP}vendor/fleece/vendor/catch
    ${TOP}vendor/fleece/vendor/jsonsl
    ${TOP}vendor/SQLiteCpp/include
    ${TOP}vendor/SQLiteCpp/sqlite3
    ${TOP}C
    ${TOP}C/include
    ${TOP}C/tests
    ${TOP}Crypto
    ${TOP}Networking
    ${TOP}Networking/BLIP
    ${TOP}Networking/HTTP
    ${TOP}Networking/WebSockets
    ${TOP}LiteCore/BlobStore
    ${TOP}LiteCore/Database
    ${TOP}LiteCore/RevTrees
    ${TOP}LiteCore/Storage
    ${TOP}LiteCore/Support
    ${TOP}LiteCore/Query
    ${TOP}Replicator
    ${TOP}Replicator/tests
    ${TOP}REST
    ${TOP}REST/tests
    ${TOP}vendor/sockpp/include
)

target_link_libraries(
    CppTests PRIVATE
    LiteCoreStatic
    LiteCoreREST_Static
    LiteCoreWebSocket
    FleeceStatic
    SQLite3_UnicodeSN
    BLIPStatic
    Support
    mbedcrypto
    mbedtls
    ${LITECORE_CRYPTO_LIB}
)
