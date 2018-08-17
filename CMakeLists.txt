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

### SOURCES:

include_directories("include/blip_cpp"
                    "src/blip"
                    "src/util"
                    "src/websocket"
                    "vendor/zlibcomplete/lib"
                    "../fleece/API"
                    "../../LiteCore/Support")

aux_source_directory("src/blip"                 BLIP_SRC)
aux_source_directory("src/util"                 UTIL_SRC)
aux_source_directory("src/websocket"            WEBSOCKET_SRC)
aux_source_directory("vendor/zlibcomplete/lib"  ZLIBCOMPLETE_SRC)

if(NOT APPLE)
    list(REMOVE_ITEM UTIL_SRC src/util/GCDMailbox.cc)
endif()

set(ALL_SRC_FILES ${BLIP_SRC} ${UTIL_SRC} ${WEBSOCKET_SRC} ${ZLIBCOMPLETE_SRC})

### LIBRARY:

add_library(BLIPStatic STATIC ${ALL_SRC_FILES})
if(WIN32 OR ANDROID)
    add_subdirectory("vendor/zlib")
    include_directories("vendor/zlib")
    include_directories("${CMAKE_CURRENT_BINARY_DIR}/vendor/zlib")
endif()
