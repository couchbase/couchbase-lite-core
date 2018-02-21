cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)
project (LiteCoreREST)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

### SUBMODULE (CivetWeb):

include_directories("../Replicator"
                    "../vendor/civetweb/include")

set(CIVETWEB_SRC "../vendor/civetweb/src/civetweb.c"
                 "civetUtils.cc")
                 
add_library(CivetWeb STATIC ${CIVETWEB_SRC})

### SOURCES:

include_directories("../C/include"
                    "../C"
                    "../vendor/fleece/Fleece"
                    "../LiteCore/Support"
                    "../LiteCore/Storage")

aux_source_directory("." REST_SRC)
list(REMOVE_ITEM REST_SRC ./LiteCoreServ.cc)

set(ALL_SRC_FILES ${REST_SRC})

### STATIC LIBRARY:

add_library(LiteCoreREST_Static STATIC ${ALL_SRC_FILES})
target_link_libraries(LiteCoreREST_Static PRIVATE CivetWeb Support)
