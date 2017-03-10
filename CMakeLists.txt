cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)
project (BLIP_Cpp)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

### SUBMODULES:

set(LIBWS_WITH_OPENSSL false)
set(LIBWS_EXTERNAL_LOOP false)
add_subdirectory(vendor/libws EXCLUDE_FROM_ALL)

include_directories("vendor/libws/src"
                    "vendor/libws/build"
                    "vendor/zlibcomplete/lib"
                    "../fleece/Fleece"
                    "../../LiteCore/Support"
                    ${LIBEVENT_INCLUDE_DIRS})

### SOURCES:

include_directories("include/blip_cpp"
                    "src/blip"
                    "src/util")

aux_source_directory("src/util"                 UTIL_SRC)
aux_source_directory("vendor/zlibcomplete/lib"  ZLIBCOMPLETE_SRC)

set(ALL_SRC_FILES "src/blip/BLIPConnection.cc"
                  "src/blip/Message.cc"
                  "src/blip/LibWSProvider.cc"
                  ${UTIL_SRC} ${ZLIBCOMPLETE_SRC})

### LIBRARY:

add_library(BLIPStatic STATIC ${ALL_SRC_FILES})
