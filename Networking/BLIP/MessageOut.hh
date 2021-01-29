//
// MessageOut.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "MessageBuilder.hh"
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
                   MessageDataSource dataSource,
                   MessageNo number);

        MessageOut(Connection *connection,
                   MessageBuilder &builder,
                   MessageNo number)
        :MessageOut(connection, (FrameFlags)0, builder.finish(), builder.dataSource, number)
        {
            _flags = builder.flags();   // finish() may update the flags, so set them after
            _onProgress = std::move(builder.onProgress);
        }

        void dontCompress()                     {_flags = (FrameFlags)(_flags & ~kCompressed);}
        void nextFrameToSend(Codec &codec, slice &dst, FrameFlags &outFlags);
        void receivedAck(uint32_t byteCount);
        bool needsAck()                         {return _unackedBytes >= kMaxUnackedBytes;}
        MessageIn* createResponse();
        void disconnected();

        // for debugging/logging:
        std::string description();
        void dump(std::ostream& out, bool withBody);
        const char* findProperty(const char *propertyName);

    private:
        static const uint32_t kMaxUnackedBytes = 128000;

        /** Manages the data (properties, body, data source) of a MessageOut. */
        class Contents {
        public:
            Contents(alloc_slice payload, MessageDataSource dataSource);
            slice& dataToSend();
            bool hasMoreDataToSend() const;
            void getPropsAndBody(slice &props, slice &body) const;
        private:
            void readFromDataSource();

            alloc_slice _payload;               // Message data (uncompressed)
            slice _unsentPayload;               // Unsent subrange of _payload
            MessageDataSource _dataSource;      // Callback that produces more data to send
            alloc_slice _dataBuffer;            // Data read from _dataSource
            slice _unsentDataBuffer;            // Unsent subrange of _dataBuffer
        };

        Connection* const _connection;          // My BLIP connection
        Contents _contents;                     // Message data
        uint32_t _uncompressedBytesSent {0};    // Number of bytes of the data sent so far
        uint32_t _bytesSent {0};                // Number of bytes transmitted (after compression)
        uint32_t _unackedBytes {0};             // Bytes transmitted for which no ack received yet
    };

} }
