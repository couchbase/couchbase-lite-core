cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)
project (LiteCoreTools)

set(TOP ${PROJECT_SOURCE_DIR}/../)

include_directories(${PROJECT_SOURCE_DIR}
                    ${TOP}C/include/
                    ${TOP}LiteCore/Support/
                    ${TOP}vendor/fleece/Fleece/)

### LITECORELOG

add_executable(litecorelog
               litecorelog.cc
               ${TOP}LiteCore/Support/StringUtil.cc
               ${TOP}LiteCore/Support/LogDecoder.cc
               ${TOP}LiteCore/Support/LogEncoder.cc)

target_link_libraries(litecorelog  Fleece "-framework CoreFoundation")


### LITECP

aux_source_directory(litecp LITECP_SRC)

add_executable(litecp
               ${LITECP_SRC}
               ToolUtils.cc)

target_link_libraries(litecp ${LITECORE_LIBRARIES_PRIVATE})

if (APPLE)
    target_link_libraries(litecp "-framework CoreFoundation")
endif()
