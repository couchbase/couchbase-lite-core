#[[
LiteCore REST library CMake project

This project builds a simple REST API for LiteCore to use in conjunction with the shared
library tests.  It is *not* meant to be used anywhere else, so don't do it!
]]#

cmake_minimum_required (VERSION 3.9)
cmake_policy(VERSION 3.9)
project (LiteCoreREST)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

### SUBMODULE (CivetWeb):

# Civetweb has been modified so heavily that at this point it is easier
# to just make a new target here
set(
    CIVETWEB_SRC
    ../vendor/civetweb/src/civetweb.c
    civetUtils.cc
)
                 
add_library(CivetWeb STATIC ${CIVETWEB_SRC})
target_compile_definitions(
    CivetWeb PRIVATE
    -DUSE_WEBSOCKET                     # Enables CivetWeb WebSocket support
    -DNO_FILES                          # No file system support in CivetWeb (unneeded)
    -DNO_CGI                            # No CGI support in CivetWeb (unneeded)
    -DNO_CACHING                        # No file caching support in CivetWeb (unneeded)
    -DUSE_IPV6                          # Enable IPv6 support in CivetWeb
)

target_include_directories(
    CivetWeb PRIVATE
    ../vendor/fleece/API
    ../vendor/fleece/Fleece/Support
    ../vendor/civetweb/include
    ../C/include
    ../LiteCore/Support
)

### SOURCES:

set(
    ALL_SRC_FILES
    c4Listener+RESTFactory.cc
    c4Listener.cc
    civetUtils.cc
    Listener.cc
    Request.cc
    Response.cc
    RESTListener+Handlers.cc
    RESTListener+Replicate.cc
    RESTListener.cc
    Server.cc
)

if(BUILD_ENTERPRISE)
    set(ALL_SRC_FILES 
        ${ALL_SRC_FILES}
        EE/CivetC4Socket_stub.cc
        EE/RESTSyncListener_stub.cc
    )
endif()

### STATIC LIBRARY:

add_library(LiteCoreREST_Static STATIC ${ALL_SRC_FILES})
target_include_directories(
    LiteCoreREST_Static PRIVATE
    .
    ../C/include
    ../C
    ../vendor/civetweb/include
    ../vendor/fleece/API
    ../vendor/fleece/Fleece/Support
    ../LiteCore/Support
)
if(BUILD_ENTERPRISE)
    target_include_directories(
        LiteCoreREST_Static PRIVATE
        ../../vendor/couchbase-lite-core-EE/Listener
        ../vendor/BLIP-Cpp/include/blip_cpp
    )
endif()

### SHARED LIBRARY:

add_library(LiteCoreREST SHARED ../LiteCore/Support/Logging_Stub.cc)
target_include_directories(
    LiteCoreREST PRIVATE
    ../vendor/fleece/API
    ../vendor/fleece/Fleece/Support
)

target_include_directories(
    LiteCoreREST INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(
    LiteCoreREST PRIVATE 
    ${WHOLE_LIBRARY_FLAG} 
    LiteCoreREST_Static 
    ${NO_WHOLE_LIBRARY_FLAG} 
)

target_link_libraries(
    LiteCoreREST PUBLIC
    LiteCore 
)

setup_rest_build()