function(set_source_files_base)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(BASE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED BASE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set(
        ${BASE_SSS_RESULT}
        ${BLIP_LOCATION}/BLIPConnection.cc
        ${BLIP_LOCATION}/Message.cc
        ${BLIP_LOCATION}/MessageBuilder.cc
        ${BLIP_LOCATION}/MessageOut.cc
        ${HTTP_LOCATION}/Headers.cc
        ${WEBSOCKETS_LOCATION}/WebSocketImpl.cc
        ${WEBSOCKETS_LOCATION}/WebSocketInterface.cc
        ${SUPPORT_LOCATION}/Actor.cc
        ${SUPPORT_LOCATION}/Async.cc
        ${SUPPORT_LOCATION}/Channel.cc
        ${SUPPORT_LOCATION}/Codec.cc
        ${SUPPORT_LOCATION}/Timer.cc
        PARENT_SCOPE
    )
endfunction()
