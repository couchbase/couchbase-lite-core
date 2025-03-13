#[[
LiteCore REST library CMake project
]]#

cmake_minimum_required (VERSION 3.9)
cmake_policy(VERSION 3.9)
project (LiteCoreREST)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

### STATIC LIBRARY:

add_library(LiteCoreREST_Objects OBJECT
    c4Listener.cc
    c4Listener_CAPI.cc
    CertRequest.cc
    DatabaseRegistry.cc
    HTTPListener.cc
    netUtils.cc
    Request.cc
    Response.cc
    Server.cc
)
target_include_directories(
    LiteCoreREST_Objects PRIVATE
    .
    ../C/include
    ../C/Cpp_include
    ../C
    ../LiteCore/Logging
    ../LiteCore/Support
    ../Crypto
    ../Networking
    ../Networking/BLIP
    ../Networking/HTTP
    ../Networking/WebSockets
    ../Replicator
    ../vendor/fleece/API
    ../vendor/fleece/Fleece/Support
    ../vendor/mbedtls/include
    ../vendor/sockpp/include
    ../vendor/fleece/vendor/date/include
)

if (BUILD_ENTERPRISE)
    set(EE_SRC ../../couchbase-lite-core-EE)
    target_sources(
        LiteCoreREST_Objects PRIVATE
        ${EE_SRC}/Listener/ReplicateTask.cc
        ${EE_SRC}/Listener/SyncListener.cc
    )
    target_include_directories(
        LiteCoreREST_Objects PRIVATE
        ${EE_SRC}/Listener
    )
endif ()

target_compile_definitions(
    LiteCoreREST_Objects PRIVATE
    LITECORE_CPP_API=1
    HAS_UNCAUGHT_EXCEPTIONS # date.h use std::uncaught_exceptions instead of std::uncaught_exception
)

target_link_libraries(
    LiteCoreREST_Objects PUBLIC
    LiteCoreObjects
)

add_library(LiteCoreREST_Static STATIC $<TARGET_OBJECTS:LiteCoreREST_Objects>)


get_directory_property(this_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} BUILDSYSTEM_TARGETS)
set(LITECORE_TARGETS ${LITECORE_TARGETS} ${this_targets} PARENT_SCOPE)
setup_rest_build()

if (APPLE)
    target_link_libraries(
            LiteCoreREST_Objects INTERFACE
            "-framework Network"
    )
    target_link_libraries(
            LiteCoreREST_Static INTERFACE
            "-framework Network"
    )
endif()
