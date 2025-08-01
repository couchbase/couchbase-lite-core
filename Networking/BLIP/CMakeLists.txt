#[[
BLIP CMake Project

This project builds a static library containing the logic for crafting and processing BLIP messages.
These types of messages are used in Couchbase Lite replication starting with Couchbase Lite 2.0.

This project produces one target, BLIPObjects.

(It has dependencies on other parts of the LiteCore source tree, so it doesn't build standalone.)

If on Android or Windows this library must be linked with the zlibstatic library from the
vendor/zlib folder (the target will be automatically added on those platforms, but the final product
must link with it via target_link_libraries)
]]#

cmake_minimum_required (VERSION 3.9...4.0)
project (BLIP_Cpp)

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)

set(BLIP_LOCATION       ${CMAKE_CURRENT_LIST_DIR})
set(WEBSOCKETS_LOCATION ${CMAKE_CURRENT_LIST_DIR}/../WebSockets)
set(HTTP_LOCATION       ${CMAKE_CURRENT_LIST_DIR}/../HTTP)
set(LITECORE_LOCATION   ${CMAKE_CURRENT_LIST_DIR}/../..)
set(LOGGING_LOCATION    ${LITECORE_LOCATION}/LiteCore/Logging)
set(SUPPORT_LOCATION    ${LITECORE_LOCATION}/LiteCore/Support)
set(FLEECE_LOCATION     ${LITECORE_LOCATION}/vendor/fleece)

if(MSVC)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_win.cmake")
elseif(APPLE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_apple.cmake")
elseif(ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_android.cmake")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_linux.cmake")
else()
    message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}!")
endif()

set_source_files(RESULT ALL_SRC_FILES)
add_library(BLIPObjects OBJECT ${ALL_SRC_FILES})

get_directory_property(this_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} BUILDSYSTEM_TARGETS)
set(LITECORE_TARGETS ${LITECORE_TARGETS} ${this_targets} PARENT_SCOPE)
setup_build()

target_include_directories(
    BLIPObjects PRIVATE
    ${BLIP_LOCATION}
    ${WEBSOCKETS_LOCATION}
    ${LOGGING_LOCATION}
    ${SUPPORT_LOCATION}
    ${FLEECE_LOCATION}/API
    ${FLEECE_LOCATION}/Fleece/Support
    ${LITECORE_LOCATION}/Crypto
    ${LITECORE_LOCATION}/Networking/HTTP
    ${LITECORE_LOCATION}/C/include
    ${LITECORE_LOCATION}/C/Cpp_include
)
