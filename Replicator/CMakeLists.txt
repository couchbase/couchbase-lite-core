cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)
project (LiteCoreReplicator)

set(TOP ${PROJECT_SOURCE_DIR}/../)

include_directories(${TOP}C/include/
                    ${TOP}LiteCore/Support
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/libb64
                    ${TOP}vendor/BLIP-Cpp/include/blip_cpp
                    ${TOP}vendor/BLIP-Cpp/src/util
                    )
                    
aux_source_directory(.  REPLICATOR_SRC)

set(SRC ${REPLICATOR_SRC}
    ${TOP}LiteCore/Support/Logging.cc
    ${TOP}LiteCore/Support/RefCounted.cc
    ${TOP}LiteCore/Support/StringUtil.cc
    )

add_library(Replicator SHARED ${SRC})
target_link_libraries(Replicator PRIVATE LiteCore FleeceStatic BLIPStatic ws)
target_link_libraries(Replicator PUBLIC z)

if (APPLE)
    set_target_properties(Replicator PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/Replicator.exp")
endif()
