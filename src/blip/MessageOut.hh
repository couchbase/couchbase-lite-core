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
    class Codec;

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

        void dontCompress()                     {_flags = (FrameFlags)(_flags & ~kCompressed);}
        void nextFrameToSend(Codec &codec, slice &dst, FrameFlags &outFlags);
        void receivedAck(uint32_t byteCount);
        bool needsAck()                         {return _unackedBytes >= kMaxUnackedBytes;}
        MessageIn* createResponse();
        void disconnected();

        void dump(std::ostream& out, bool withBody);

    private:
        void readFromDataSource();

        static const uint32_t kMaxUnackedBytes = 128000;

        Connection* const _connection;      // My BLIP connection
        alloc_slice _payload;               // Message data (uncompressed)
        slice _unsentPayload;               // Unsent subrange of _payload
        MessageDataSource _dataSource;      // Callback that produces more data to send
        alloc_slice _dataBuffer;            // Data read from _dataSource
        slice _dataBufferAvail;
        bool _dataSourceMoreComing {true};
        uint32_t _bytesSent {0};            // Number of bytes transmitted (after compression)
        uint32_t _unackedBytes {0};         // Bytes transmitted but no ack received
    };

} }
