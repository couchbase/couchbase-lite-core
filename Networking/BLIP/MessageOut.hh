//
// MessageOut.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "MessageBuilder.hh"
#include "slice_stream.hh"
#include <ostream>
#include <utility>

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
                   MessageDataSource&& dataSource,
                   MessageNo number);

        MessageOut(Connection *connection,
                   BuiltMessage &&built,
                   MessageNo number);

        void dontCompress()                     {_flags = (FrameFlags)(_flags & ~kCompressed);}
        void nextFrameToSend(Codec &codec, fleece::slice_ostream &dst, FrameFlags &outFlags);
        void receivedAck(uint32_t byteCount);
        bool needsAck()                         {return _unackedBytes >= kMaxUnackedBytes;}
        MessageIn* createResponse();
        void disconnected();

        // for debugging/logging:
        std::string description();
        void dump(std::ostream& out, bool withBody);
        const char* findProperty(const char *propertyName);

    private:
        using slice_istream = fleece::slice_istream;

        std::pair<slice,slice> getPropsAndBody() const;

        static const uint32_t kMaxUnackedBytes = 128000;

        /** Manages the data (properties, body, data source) of a MessageOut. */
        class Contents {
        public:
            Contents(alloc_slice payload, MessageDataSource dataSource);
            slice_istream& dataToSend();
            bool hasMoreDataToSend() const;
            std::pair<slice,slice> getPropsAndBody() const;
            slice body() const                  {return _payload;}
        private:
            void readFromDataSource();

            alloc_slice _payload;               // Message data (uncompressed)
            slice_istream _unsentPayload;       // Unsent subrange of _payload
            MessageDataSource _dataSource;      // Callback that produces more data to send
            alloc_slice _dataBuffer;            // Data read from _dataSource
            slice_istream _unsentDataBuffer;    // Unsent subrange of _dataBuffer
        };

        Connection* const _connection;          // My BLIP connection
        Contents _contents;                     // Message data
        uint32_t _uncompressedBytesSent {0};    // Number of bytes of the data sent so far
        uint32_t _bytesSent {0};                // Number of bytes transmitted (after compression)
        uint32_t _unackedBytes {0};             // Bytes transmitted for which no ack received yet
    };

} }
