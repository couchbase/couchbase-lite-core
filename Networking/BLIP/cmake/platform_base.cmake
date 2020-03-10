function(set_source_files_base)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(BASE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED BASE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set(
        ${BASE_SSS_RESULT}
        src/blip/BLIPConnection.cc
        src/blip/Message.cc
        src/blip/MessageBuilder.cc
        src/blip/MessageOut.cc
        src/util/Actor.cc
        src/util/ActorProperty.cc
        src/util/Async.cc
        src/util/Channel.cc
        src/util/Codec.cc
        src/util/Timer.cc
        src/websocket/Headers.cc
        src/websocket/WebSocketImpl.cc
        src/websocket/WebSocketInterface.cc
        PARENT_SCOPE
    )
endfunction()