//
//  MessageOut.hh
//  blip_cpp
//
//  Created by Jens Alfke on 4/5/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "MessageBuilder.hh"
#include <ostream>

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
                   MessageDataSource dataSource,
                   MessageNo number);

        MessageOut(Connection *connection,
                   MessageBuilder &builder,
                   MessageNo number)
        :MessageOut(connection, (FrameFlags)0, builder.extractOutput(), builder.dataSource, number)
        {
            _flags = builder.flags();   // extractOutput() may update the flags, so set them after
            _onProgress = std::move(builder.onProgress);
        }

        fleece::slice nextFrameToSend(size_t maxSize, FrameFlags &outFlags);
        void receivedAck(uint32_t byteCount);
        bool needsAck()                         {return _unackedBytes >= kMaxUnackedBytes;}
        MessageIn* createResponse();
        void disconnected();

        void dump(std::ostream& out, bool withBody);

    private:
        static const uint32_t kMaxUnackedBytes = 128000;

        Connection* const _connection;
        fleece::alloc_slice _payload;
        MessageDataSource _dataSource;
        alloc_slice _dataBuffer;
        uint32_t _bytesSent {0};
        uint32_t _unackedBytes {0};
    };

} }
