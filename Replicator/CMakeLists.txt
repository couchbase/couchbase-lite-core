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

if (APPLE)
    include_directories("Obj-C")
    set(SRC ${SRC}
        "Obj-C/CBLWebSocket.mm"
        "Obj-C/CBLHTTPLogic.m"
    )
elseif(WIN32)
    set(SRC ${SRC}
        "../MSVC/arc4random.c"
        "../MSVC/vasprintf-msvc.c"
        "../MSVC/asprintf.c"
    )
endif()

add_library(Replicator SHARED ${SRC})
target_link_libraries(Replicator PRIVATE LiteCore FleeceStatic BLIPStatic)
if(WIN32)
    target_link_libraries(Replicator PRIVATE zlibstatic Ws2_32)
    set_target_properties(Replicator PROPERTIES LINK_FLAGS
			  "/def:${PROJECT_SOURCE_DIR}/Replicator.def")
else()
    target_link_libraries(Replicator PUBLIC z)
endif()

if (APPLE)
    target_link_libraries(Replicator PUBLIC "-framework CoreFoundation"
                                            "-framework CFNetwork"
                                            "-framework Foundation"
                                            "-framework Security")
    set_target_properties(Replicator PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/Replicator.exp")
else()
    target_compile_definitions(zlibstatic PUBLIC -D_CRYPTO_OPENSSL)
    target_link_libraries(Replicator PRIVATE ${OPENSSL_CRYPTO_LIBRARY})
endif()
