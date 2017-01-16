//
//  BLIPInternal.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/4/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Message.hh"

namespace litecore { namespace blip {

    /** An outgoing message that's been constructed by a MessageBuilder. */
    class MessageOut : public Message {
    protected:
        friend class MessageIn;
        friend class Connection;
        friend class BLIPIO;

        MessageOut(Connection *connection,
                   FrameFlags flags,
                   alloc_slice payload,
                   MessageNo number);

        MessageOut(Connection *connection, MessageBuilder &builder, MessageNo number =0)
        :MessageOut(connection, builder.flags(), builder.extractOutput(), number)
        { }

        fleece::slice nextFrameToSend(size_t maxSize, FrameFlags &outFlags);
        void receivedAck(uint32_t byteCount);
        bool needsAck()                         {return _unackedBytes >= kMaxUnackedBytes;}
        MessageIn* pendingResponse();

    private:
        static const uint32_t kMaxUnackedBytes = 128000;

        Connection* const _connection;
        fleece::alloc_slice _payload;
        uint32_t _bytesSent {0};
        uint32_t _unackedBytes {0};
        Retained<MessageIn> _pendingResponse;
    };

} }
