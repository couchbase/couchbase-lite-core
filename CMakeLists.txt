#[[
LiteCore CMake Project

This is the CMake project for building the Couchbase LiteCore project.  This is the core of the Couchbase Lite library.
It makes use of a lot of source and sublibraries and some cannot be used on their own.  Here are a description of the
targets that result from this project:

BLIPObjects - The BLIP communication library for crafting messages that can be sent over a provided connection
C4Tests - A test runner that runs tests based on the shared library
CppTests - A test runner that runs test based on the static library
FleeceObjects - The Fleece serialization library for saving data to a binary format
FleeceBase - A subset of the above for linking into non LiteCore targets
LiteCoreObjects - The precompiled object files containing the logic of LiteCore
LiteCoreUnitTesting - The precompiled object files containing the logic of LiteCore for unit testing.
LiteCoreStatic - The static LiteCore library (LiteCoreObjects linked statically)
LiteCore - The shared LiteCore library (LiteCoreObjects linked dynamically)
LiteCoreREST - A simple library used for enabling testing of the shared library (not used in production)
LiteCoreWebSocket - An implementation of websockets in C++ (Used for EE library, and can be used downstream by other projects)
mbedcrypto - The cryptography suite from mbedTLS (https://tls.mbed.org/)
SQLite3_UnicodeSN - The snowball tokenizer library for SQLite

This project is built for the following platforms at Couchbase:
- Windows 10
- UWP
- macOS 13.0
- CentOS 7 (gcc 7.x+)
- Android API 22+

Platform logic is largely separated into the cmake/platform_*.cmake files.  Platforms are conglomerated together as follows
- platform_base
    - platform_unix
        - platform_apple
        - platform_linux
            - platform_linux_desktop
            - platform_android
    - platform_win
        - platform_win_desktop
]]#

cmake_minimum_required (VERSION 3.21...4.0)

# Mac/apple setup -- must appear before the first "project()" line"
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
if(NOT DEFINED CMAKE_OSX_SYSROOT)
    # Tells Mac builds to use the current SDK's headers & libs, not what's in the OS.
    set(CMAKE_OSX_SYSROOT macosx)
endif()

option(LITECORE_MACOS_FAT_DEBUG "Builds all architectures for a debug build (off by default for speed)" OFF)

if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    if (NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug") OR LITECORE_MACOS_FAT_DEBUG)
        set(CMAKE_OSX_ARCHITECTURES x86_64;arm64 CACHE STRING "" FORCE)
    endif()
endif()

if(DEFINED ENV{VERSION})
    message(VERBOSE "Using VERSION:$ENV{VERSION} from environment variable")
    set(CBL_VERSION_STRING $ENV{VERSION})
else()
    message(WARNING "No VERSION set, defaulting to 0.0.0")
    set(CBL_VERSION_STRING "0.0.0")
endif()

project (
    LiteCore
    VERSION ${CBL_VERSION_STRING}
)
include(CMakeDependentOption)

### BUILD SETTINGS:

set(GENERATED_HEADERS_DIR "${CMAKE_BINARY_DIR}/generated_headers")
file(MAKE_DIRECTORY "${GENERATED_HEADERS_DIR}")
include(cmake/generate_edition.cmake)
generate_edition()

set(LITECORE_TARGETS "")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)

option(CODE_COVERAGE_ENABLED "Set whether or not code coverage information should be generated" OFF)
option(LITECORE_PERF_TESTING_MODE "Build LiteCore with more things public than in production to facilitate perf testing" OFF)
option(BUILD_ENTERPRISE "Set whether or not to build enterprise edition" OFF)
option(LITECORE_DISABLE_ICU "Disables ICU linking" OFF)
option(DISABLE_LTO_BUILD "Disable build with Link-time optimization" OFF)
option(LITECORE_BUILD_TESTS "Builds C4Tests and CppTests" ON)
option(LITECORE_WARNINGS_HARDCORE "Enables tons of warnings and makes them errors (Clang only)" OFF)
option(LITECORE_SANITIZE "Enables address and undefined-behavior sanitizers (Clang only)" OFF)
option(LITECORE_BUILD_SHARED "Enables building the LiteCore shared library" ON)
option(LITECORE_BUILD_STATIC "Enables building the LiteCore static library" ON)

set(LITECORE_PREBUILT_LIB "" CACHE STRING "If set, C4Tests will use the prebuilt LiteCore instead of building it from source")

option(LITECORE_MAINTAINER_MODE "Build the library with official options, disable this to reveal additional options" ON)

# The following will only show up if LITECORE_MAINTAINER_MODE is disabled, in other words if LITECORE_MAINTAINER_MODE is on
# the following options will be ON, but if LITECORE_MAINTAINER_MODE is off, the the following options can be
# selected on or off.  Standard warning about dragons!
cmake_dependent_option(
    USE_COUCHBASE_SQLITE
    "If enabled, the Couchbase provided SQLite will be built"
    OFF "NOT LITECORE_MAINTAINER_MODE" ON
)

if(CODE_COVERAGE_ENABLED)
   message("Code coverage enabled, forcing sanitizers off")
   if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" AND "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
     set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
   elseif(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
     message(WARN " Code coverage not supported for non-debug builds")
   else()
     message(WARN " Code coverage only supported on Clang")
   endif()
endif()

add_definitions(
    -DCMAKE                  # Let the source know this is a CMAKE build
    -D__STDC_FORMAT_MACROS   # Enables printf format macros for variable sized types (e.g. size_t)
    -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES # Stop libc++ headers from including extra headers
)

if(BUILD_ENTERPRISE)
    add_definitions(
        -DCOUCHBASE_ENTERPRISE      # Tells LiteCore it's an EE build
    )
endif()

if(MSVC)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00)

    # CBL-6931 : Workaround to avoid crash when locking mutex on VC++ Runtime version < 14.4.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

    if(WINDOWS_STORE)
        message(FATAL_ERROR "UWP no longer supported")
    else()
        include("${PROJECT_SOURCE_DIR}/cmake/platform_win_desktop.cmake")
    endif()
elseif(APPLE)
    include("${PROJECT_SOURCE_DIR}/cmake/platform_apple.cmake")
elseif(ANDROID)
    include("${PROJECT_SOURCE_DIR}/cmake/platform_android.cmake")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    option(LITECORE_DYNAMIC_ICU "If enabled, search for ICU at runtime so as not to depend on a specific version" OFF)
    include("${PROJECT_SOURCE_DIR}/cmake/platform_linux_desktop.cmake")
else()
    message(FATAL_ERROR "Unable to determine a supported platform from ${CMAKE_SYSTEM_NAME}")
endif(MSVC)

check_threading()
setup_globals()
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LITECORE_SHARED_LINKER_FLAGS}")
set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} ${LITECORE_STATIC_LINKER_FLAGS}")
set(SQLITE3_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/SQLiteCpp/sqlite3) # For SQLite3_UnicodeSN

if (LITECORE_WARNINGS_HARDCORE)
    set(FLEECE_WARNINGS_HARDCORE ON CACHE INTERNAL "")
endif ()

add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)

set(ENABLE_PROGRAMS OFF CACHE INTERNAL "Build mbed TLS programs.")
set(ENABLE_TESTING OFF CACHE INTERNAL "Build mbed TLS tests.")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE INTERNAL "") # Work around doc-comment syntax warnings
add_subdirectory(vendor/mbedtls             EXCLUDE_FROM_ALL)

configure_file(cmake/config_thread.h.in ${GENERATED_HEADERS_DIR}/config_thread.h)
include_directories(${GENERATED_HEADERS_DIR})

add_subdirectory(Networking/BLIP            EXCLUDE_FROM_ALL)

if (BUILD_ENTERPRISE)
    add_subdirectory(../couchbase-lite-core-EE/Listener EE_Listener EXCLUDE_FROM_ALL)
    add_subdirectory(../couchbase-lite-core-EE/P2P      EE_P2P      EXCLUDE_FROM_ALL)
endif()

### sqlite3 LIBRARY:

# Separate library to make possible replace or not use
# during static linking
add_library(CouchbaseSqlite3 STATIC LiteCore/Storage/SQLiteChooser.c)
target_include_directories(CouchbaseSqlite3 PUBLIC vendor/SQLiteCpp/sqlite3)
target_compile_definitions(
  CouchbaseSqlite3
  PUBLIC
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
    -DSQLITE_ENABLE_FTS4                # Build FTS versions 3 and 4
    -DSQLITE_ENABLE_FTS3_PARENTHESIS    # Allow AND and NOT support in FTS parser
    -DSQLITE_ENABLE_FTS3_TOKENIZER      # Allow LiteCore to define a tokenizer
    -DSQLITE_PRINT_BUF_SIZE=200         # Extend the print buffer size to get more descriptive messages
    -DSQLITE_OMIT_DEPRECATED            # Don't compile in deprecated functionality
    -DSQLITE_DQS=0                      # Disallow double-quoted strings (only identifiers)
)

### LITECORE LIBRARY:

set_litecore_source(RESULT ALL_SRC_FILES)
add_library(LiteCoreObjects OBJECT ${ALL_SRC_FILES})
add_library(LiteCoreUnitTesting OBJECT EXCLUDE_FROM_ALL ${ALL_SRC_FILES})
set(LiteCoreObjectsDefines
    LITECORE_IMPL
    LITECORE_CPP_API=1
    HAS_UNCAUGHT_EXCEPTIONS # date.h use std::uncaught_exceptions instead of std::uncaught_exception
)
target_compile_definitions(
    LiteCoreObjects PRIVATE
    ${LiteCoreObjectsDefines}
)
target_compile_definitions(
    LiteCoreUnitTesting PRIVATE
    ${LiteCoreObjectsDefines}
    PUBLIC LITECORE_CPPTEST
)


if(BUILD_ENTERPRISE)
    target_compile_definitions(CouchbaseSqlite3
         PRIVATE
        -DSQLITE_HAS_CODEC              # Enables SQLite encryption extension (SEE)
    )
endif()

set(
    LiteCoreObjectsIncludes
    vendor/fleece/API
    vendor/fleece/Fleece/Core
    vendor/fleece/Fleece/Mutable
    vendor/fleece/Fleece/Support
    vendor/fleece/vendor/date/include
    LiteCore/BlobStore
    LiteCore/Database
    LiteCore/Logging
    LiteCore/Query
    LiteCore/Query/N1QL_Parser
    LiteCore/Query/Translator
    LiteCore/RevTrees
    LiteCore/Storage
    LiteCore/Support
    C
    C/include
    C/Cpp_include
    Crypto
    Networking
    Networking/BLIP/
    Networking/HTTP
    Networking/WebSockets
    Replicator
    REST
    vendor/SQLiteCpp/include
    vendor/SQLiteCpp/sqlite3/ext
    vendor/sqlite3-unicodesn
    vendor/mbedtls/include
    vendor/sockpp/include
    vendor/vector_search
)

target_include_directories(
    LiteCoreObjects PRIVATE
    ${LiteCoreObjectsIncludes}
)
target_include_directories(
    LiteCoreUnitTesting PRIVATE
    ${LiteCoreObjectsIncludes}
)

# Library flags defined in platform_linux
set(
    LITECORE_LIBRARIES_PRIVATE
    LiteCoreObjects
    BLIPObjects
    FleeceObjects
)

if(BUILD_ENTERPRISE)
    set(
        LITECORE_LIBRARIES_PRIVATE
        ${LITECORE_LIBRARIES_PRIVATE}
        LiteCoreREST_Objects
        LiteCoreWebSocket
        LiteCoreListener_Objects
        LiteCoreP2P
    )
endif()

if(LITECORE_BUILD_SHARED)
    if(MSVC)
        add_library(LiteCore SHARED $<TARGET_OBJECTS:LiteCoreObjects> MSVC/dllmain.c)
        target_include_directories(
            LiteCore PRIVATE
            C/include
        )
    else()
        add_library(LiteCore SHARED $<TARGET_OBJECTS:LiteCoreObjects>)
    endif()

    target_include_directories(
        LiteCore INTERFACE
        C/include
        C/Cpp_include
    ) 
    target_link_libraries(LiteCore PRIVATE ${LITECORE_LIBRARIES_PRIVATE})

    install (
        TARGETS LiteCore
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        OPTIONAL
    )

    file(GLOB LITECORE_HEADERS ${PROJECT_SOURCE_DIR}/C/include/*.h ${PROJECT_SOURCE_DIR}/C/Cpp_include/*.hh)
    file(GLOB FLEECE_HEADERS ${PROJECT_SOURCE_DIR}/vendor/fleece/API/fleece/*.h ${PROJECT_SOURCE_DIR}/vendor/fleece/API/fleece/*.hh)

    install(FILES ${LITECORE_HEADERS} DESTINATION include)
    install(FILES ${FLEECE_HEADERS} DESTINATION include/fleece)
endif()

if(LITECORE_BUILD_STATIC)
    add_library(LiteCoreStatic STATIC EXCLUDE_FROM_ALL $<TARGET_OBJECTS:LiteCoreObjects>)
    target_link_libraries(LiteCoreStatic PRIVATE LiteCoreObjects)
endif()


target_link_libraries(
    LiteCoreObjects INTERFACE
    SQLite3_UnicodeSN
    mbedcrypto
    mbedtls
    mbedx509
)
target_link_libraries(
    LiteCoreUnitTesting INTERFACE
    SQLite3_UnicodeSN
    mbedcrypto
    mbedtls
    mbedx509
)

if(USE_COUCHBASE_SQLITE)
    target_link_libraries(
        LiteCoreObjects PUBLIC
        CouchbaseSqlite3
    )
    target_link_libraries(
        LiteCoreUnitTesting PUBLIC
        CouchbaseSqlite3
    )
endif()

### Support Libraries (Add functionality, but add nothing to official API)

add_subdirectory(REST EXCLUDE_FROM_ALL)

set(
    LC_WEBSOCKET_SRC
    Networking/HTTP/HTTPTypes.cc
    Networking/HTTP/HTTPLogic.cc
    Networking/NetworkInterfaces.cc
    Networking/Poller.cc
    Networking/TCPSocket.cc
    Networking/TLSContext.cc
    Networking/WebSockets/BuiltInWebSocket.cc
    vendor/sockpp/src/acceptor.cpp
    vendor/sockpp/src/connector.cpp
    vendor/sockpp/src/datagram_socket.cpp
    vendor/sockpp/src/exception.cpp
    vendor/sockpp/src/inet_address.cpp
    vendor/sockpp/src/inet6_address.cpp
    vendor/sockpp/src/mbedtls_context.cpp
    vendor/sockpp/src/socket.cpp
    vendor/sockpp/src/stream_socket.cpp
)

add_library(LiteCoreWebSocket STATIC EXCLUDE_FROM_ALL ${LC_WEBSOCKET_SRC})
target_include_directories(
    LiteCoreWebSocket PRIVATE
    C
    C/include
    C/Cpp_include
    Crypto
    LiteCore/Database
    LiteCore/Logging
    LiteCore/Support
    Networking
    Networking/BLIP/
    Networking/HTTP
    Networking/WebSockets
    Replicator
    REST
    vendor/fleece/Fleece/Support
    vendor/fleece/API
    vendor/sockpp/include
    vendor/mbedtls/include
)

target_link_libraries(
    LiteCoreWebSocket PUBLIC
    LiteCoreObjects
)

if(LITECORE_PERF_TESTING_MODE)
    target_compile_definitions(
        LiteCoreWebSocket PUBLIC
        LITECORE_PERF_TESTING_MODE
    )
endif()


add_subdirectory(tool_support EXCLUDE_FROM_ALL)


### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)

get_directory_property(this_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} BUILDSYSTEM_TARGETS)
set(LITECORE_TARGETS ${LITECORE_TARGETS} ${this_targets})
setup_litecore_build()
