#[[
LiteCore CMake Project

This is the CMake project for building the Couchbase LiteCore project.  This is the core of the Couchbase Lite library.
It makes use of a lot of source and sublibraries and some cannot be used on their own.  Here are a description of the
targets that result from this project:

BLIPStatic - The BLIP communication library for crafting messages that can be sent over a provided connection
C4Tests - A test runner that runs tests based on the shared library
CivetWeb - A C++ implementation of the websocket client and server protocol (non-secure only)
CppTests - A test runner that runs test based on the static library
FleeceStatic - The Fleece serialization library for saving data to a binary format
LiteCore - The shared LiteCore library
LiteCoreREST - A simple library used for enabling testing of the shared library (not used in production)
LiteCoreStatic - The static LiteCore library
mbedcrypto - The cryptography suite from mbedTLS (https://tls.mbed.org/)
SQLite3_UnicodeSN - The snowball tokenizer library for SQLite
Support - A library of shared functionality that is used in some or all of the other targets

This project is built for the following platforms at Couchbase:
- Windows 10
- UWP
- macOS 10.13
- CentOS 7 (clang / libc++)
- Android API 19+

Platform logic is largely separated into the cmake/platform_*.cmake files.  Platforms are conglomerated together as follows
- platform_base
    - platform_unix
        - platform_apple
        - platform_linux
            - platform_linux_desktop
            - platform_android
    - platform_win
        - platform_win_desktop
        - platform_win_store
]]#

cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)

if(NOT DEFINED CMAKE_OSX_SYSROOT)
    # Tells Mac builds to use the current SDK's headers & libs, not what's in the OS.
    set(CMAKE_OSX_SYSROOT macosx)      # this line MUST be above the 1st "project()" line!
endif()

project (LiteCore)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)

option(CODE_COVERAGE_ENABLED "Set whether or not code coverage information should be generated" OFF)
option(BUILD_ENTERPRISE "Set whether or not to build enterprise edition" OFF)
option(LITECORE_DISABLE_ICU "Disables ICU linking" OFF)

add_definitions(
    -DCMAKE                             # Let the source know this is a CMAKE build
)

if(BUILD_ENTERPRISE)
    add_definitions(
        -DCOUCHBASE_ENTERPRISE      # Tells LiteCore it's an EE build
    )         
endif()

if(MSVC)
    if(WINDOWS_STORE)
        include("${PROJECT_SOURCE_DIR}/cmake/platform_win_store.cmake")
    else()
        include("${PROJECT_SOURCE_DIR}/cmake/platform_win_desktop.cmake")
    endif()
elseif(APPLE)
    include("${PROJECT_SOURCE_DIR}/cmake/platform_apple.cmake")
elseif(ANDROID)
    include("${PROJECT_SOURCE_DIR}/cmake/platform_android.cmake")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    include("${PROJECT_SOURCE_DIR}/cmake/platform_linux_desktop.cmake")
else()
    message(FATAL_ERROR "Unable to determine a supported platform from ${CMAKE_SYSTEM_NAME}")
endif(MSVC)

if(CODE_COVERAGE_ENABLED)
  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" AND "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -O0")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0")
  elseif(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    message(WARN " Code coverage not supported for non-debug builds")
  else()
    message(WARN " Code coverage only supported on Clang")
  endif()
endif()

setup_globals()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LITECORE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LITECORE_CXX_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LITECORE_SHARED_LINKER_FLAGS}")
set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} ${LITECORE_STATIC_LINKER_FLAGS}")
set(SQLITE3_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/SQLiteCpp/sqlite3) # For SQLite3_UnicodeSN

add_subdirectory(REST                       EXCLUDE_FROM_ALL)
add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)
add_subdirectory(vendor/BLIP-Cpp            EXCLUDE_FROM_ALL)
add_subdirectory(vendor/mbedtls             EXCLUDE_FROM_ALL)

# Generate file repo_version.h containing Git repo information, and add it to #include path:
set(GENERATED_HEADERS_DIR "${CMAKE_BINARY_DIR}/generated_headers")
file(MAKE_DIRECTORY "${GENERATED_HEADERS_DIR}")
if (UNIX)
    execute_process(COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/build_cmake/scripts/get_repo_version.sh"
                                      "${GENERATED_HEADERS_DIR}/repo_version.h")
else()
    execute_process(COMMAND powershell "${PROJECT_SOURCE_DIR}/build_cmake/scripts/get_repo_version.ps1"
                                      "${GENERATED_HEADERS_DIR}/repo_version.h")
endif()
include_directories(${GENERATED_HEADERS_DIR})

### SUPPORT LIBRARY:

set_support_source(RESULT SUPPORT_SRC)
add_library(
    Support STATIC
    ${SUPPORT_SRC}
)

target_compile_definitions(
    Support PRIVATE
    -DNOMINMAX
)

target_include_directories(
    Support PRIVATE
    vendor/fleece/API
    vendor/fleece/Fleece/Support
    LiteCore/BlobStore
    LiteCore/Database
    LiteCore/Support
    LiteCore/Storage
    LiteCore/Query
    LiteCore/RevID
    vendor/mbedtls/include
    C
    C/include 
    vendor/BLIP-Cpp/include/blip_cpp
    vendor/BLIP-Cpp/src/util
    vendor/BLIP-Cpp/src/websocket
    vendor/SQLiteCpp/sqlite3
    vendor/SQLiteCpp/include
)
setup_support_build()

### LITECORE LIBRARY:

set_litecore_source(RESULT ALL_SRC_FILES)
add_library(LiteCoreStatic STATIC ${ALL_SRC_FILES})
target_compile_definitions(
    LiteCoreStatic PRIVATE 
    -DLITECORE_IMPL
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1  # Default to NORMAL sync mode on SQLite (safe with WAL mode)
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS    # Optimize SQLite "like" queries
    -DSQLITE_OMIT_SHARED_CACHE          # Allows SQLite to discard shared cache, eliminating some branches
    -DSQLITE_OMIT_DECLTYPE              # Disable returning the declared column type from query (reduces memory usage)
    -DSQLITE_OMIT_DATETIME_FUNCS        # Don't compile SQLite date functions
    -DSQLITE_ENABLE_EXPLAIN_COMMENTS    # Add comment text to the result of sqlite3_explain
    -DSQLITE_DISABLE_FTS3_UNICODE       # Disable FTS3 unicode61 tokenizer (not used in LiteCore)
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT   # Enable sqlite3_release_memory to release unused memory faster
    -DSQLITE_ENABLE_STAT4               # Enable enhanced query planning
    -DSQLITE_HAVE_ISNAN                 # Use system provided isnan()
    -DHAVE_LOCALTIME_R                  # Use localtime_r instead of localtime
    -DHAVE_USLEEP                       # Allow millisecond precision sleep
    -DHAVE_UTIME                        # Use utime() instead of utimes()
    -DSQLITE_OMIT_LOAD_EXTENSION        # Disable extensions (not needed for LiteCore)
    -DSQLITE_ENABLE_FTS4                # Build FTS versions 3 and 4
    -DSQLITE_ENABLE_FTS3_PARENTHESIS    # Allow AND and NOT support in FTS parser
    -DSQLITE_ENABLE_FTS3_TOKENIZER      # Allow LiteCore to define a tokenizer
)

if(BUILD_ENTERPRISE)
    target_compile_definitions(
        LiteCoreStatic PRIVATE
        -DSQLITE_HAS_CODEC              # Enables SQLite encryption extension (SEE)
    )   
endif()

target_include_directories(
    LiteCoreStatic PRIVATE
    vendor/fleece/API
    vendor/fleece/Fleece/Core
    vendor/fleece/Fleece/Mutable
    vendor/fleece/Fleece/Support
    LiteCore/BlobStore
    LiteCore/Database
    LiteCore/Query
    LiteCore/RevTrees
    LiteCore/Storage
    LiteCore/Support
    vendor/mbedtls/include
    C
    C/include 
    vendor/BLIP-Cpp/include/blip_cpp
    vendor/BLIP-Cpp/src/util
    vendor/SQLiteCpp/sqlite3
    vendor/SQLiteCpp/include
    vendor/sqlite3-unicodesn
    vendor/civetweb/include
)

if(WIN32)
    add_library(LiteCore SHARED MSVC/SQLiteTempDirectory.cc) #No-op for non-UWP
    target_include_directories(
        LiteCore PRIVATE
        vendor/SQLiteCpp/sqlite3
    )
else()
    file(WRITE empty.cpp)
    add_library(LiteCore SHARED empty.cpp)
endif()

# Library flags defined in platform_linux
# LITECORE_CRYPTO_LIB defined in platform CMake files
set(
    LITECORE_LIBRARIES_PRIVATE  
    ${WHOLE_LIBRARY_FLAG}
    LiteCoreStatic
    FleeceStatic
    ${NO_WHOLE_LIBRARY_FLAG}
    Support
    SQLite3_UnicodeSN
    BLIPStatic
    CivetWeb
    ${LITECORE_CRYPTO_LIB}
)

target_include_directories(
    LiteCore INTERFACE
    C/include
)
target_link_libraries(LiteCore PRIVATE ${LITECORE_LIBRARIES_PRIVATE})
setup_litecore_build()

install (
    TARGETS LiteCore
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    OPTIONAL
)

### JNI:

if (JNI)
    add_subdirectory(Java)
endif()

### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
