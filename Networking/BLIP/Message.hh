//
// Message.hh
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
#include "BLIPProtocol.hh"
#include "fleece/RefCounted.hh"
#include "fleece/Fleece.hh"
#include "slice_stream.hh"
#include <functional>
#include <ostream>
#include <memory>
#include <mutex>

namespace fleece {
    class Value;
}

namespace litecore { namespace blip {
        using fleece::RefCounted;
        using fleece::Retained;

        class Connection;
        class MessageBuilder;
        class MessageIn;
        class InflaterWriter;
        class Codec;

        /** Progress notification for an outgoing request. */
        struct MessageProgress {
            enum State {
                kQueued,          // Outgoing request has been queued for delivery
                kSending,         // First bytes of message have been sent
                kAwaitingReply,   // Message sent; waiting for a reply (unless noreply)
                kReceivingReply,  // Reply is being received
                kComplete,        // Delivery (and receipt, if not noreply) complete.
                kDisconnected     // Socket disconnected before delivery or receipt completed
            } state;

            MessageSize         bytesSent;
            MessageSize         bytesReceived;
            Retained<MessageIn> reply;
        };

        using MessageProgressCallback = std::function<void(const MessageProgress&)>;

        struct Error {
            const fleece::slice domain;
            const int           code{0};
            const fleece::slice message;

            Error() = default;

            Error(fleece::slice domain_, int code_, fleece::slice msg = fleece::nullslice)
                : domain(domain_), code(code_), message(msg) {}
        };

        // Like Error but with an allocated message string
        struct ErrorBuf : public Error {
            ErrorBuf() = default;

            ErrorBuf(fleece::slice domain, int code, fleece::alloc_slice msg)
                : Error(domain, code, msg), _messageBuf(msg) {}

          private:
            const fleece::alloc_slice _messageBuf;
        };

        /** Abstract base class of messages */
        class Message : public RefCounted {
          public:
            using slice       = fleece::slice;
            using alloc_slice = fleece::alloc_slice;

            bool isResponse() const { return type() >= kResponseType; }

            bool isError() const { return type() == kErrorType; }

            bool urgent() const { return hasFlag(kUrgent); }

            bool noReply() const { return hasFlag(kNoReply); }

            MessageNo number() const { return _number; }

          protected:
            friend class BLIPIO;

            Message(FrameFlags f, MessageNo n) : _flags(f), _number(n) {
                /*Log("NEW Message<%p, %s #%llu>", this, typeName(), _number);*/
            }

            virtual ~Message() {
                //Log("DELETE Message<%p, %s #%llu>", this, typeName(), _number);
            }

            FrameFlags flags() const { return _flags; }

            bool hasFlag(FrameFlags f) const { return (_flags & f) != 0; }

            bool isAck() const { return type() == kAckRequestType || type() == kAckResponseType; }

            virtual bool isIncoming() const { return false; }

            MessageType type() const { return (MessageType)(_flags & kTypeMask); }

            const char* typeName() const { return kMessageTypeNames[type()]; }

            void sendProgress(MessageProgress::State state, MessageSize bytesSent, MessageSize bytesReceived,
                              MessageIn* reply);
            void disconnected();

            void dump(slice payload, slice body, std::ostream&);
            void dumpHeader(std::ostream&);
            void writeDescription(slice payload, std::ostream&);

            static const char* findProperty(slice payload, const char* propertyName);

            FrameFlags              _flags;
            MessageNo               _number;
            MessageProgressCallback _onProgress;
        };

        /** An incoming message. */
        class MessageIn : public Message {
          public:
            /** Gets a property value */
            slice property(slice property) const;
            long  intProperty(slice property, long defaultValue = 0) const;
            bool  boolProperty(slice property, bool defaultValue = false) const;

            /** Returns information about an error (if this message is an error.) */
            Error getError() const;

            void setProgressCallback(MessageProgressCallback callback);

            /** Returns true if the message has been completely received including the body. */
            bool isComplete() const;

            /** The body of the message. */
            alloc_slice body() const;

            /** Returns the body, removing it from the message. The next call to extractBody() or
            body() will return only the data that's been read since this call. */
            alloc_slice extractBody();

            /** Converts the body from JSON to Fleece and returns a pointer to the root object. */
            fleece::Value JSONBody();

            /** Sends a response. (The message must be complete.) */
            void respond(MessageBuilder&);

            /** Sends an empty default response, unless the request was sent noreply.
            (The message must be complete.) */
            void respond();

            /** Sends an error as a response. (The message must be complete.) */
            void respondWithError(Error);

            /** Responds with an error saying that the message went unhandled.
            Call this if you don't know what to do with a request.
            (The message must be complete.) */
            void notHandled();

            void dump(std::ostream& out, bool withBody) {
                Message::dump(_properties, (withBody ? _body : fleece::alloc_slice()), out);
            }

          protected:
            friend class MessageOut;
            friend class BLIPIO;

            enum ReceiveState { kOther, kBeginning, kEnd };

            MessageIn(Connection*, FrameFlags, MessageNo, MessageProgressCallback = nullptr,
                      MessageSize outgoingSize = 0);
            virtual ~MessageIn();

            virtual bool isIncoming() const { return true; }

            ReceiveState receivedFrame(Codec&, slice frame, FrameFlags);

            std::string description();

          private:
            void readFrame(Codec&, int mode, fleece::slice_istream& frame, bool finalFrame);
            void acknowledge(uint32_t frameSize);

            Retained<Connection>                 _connection;  // The owning BLIP connection
            mutable std::mutex                   _receiveMutex;
            MessageSize                          _rawBytesReceived{0};
            std::unique_ptr<fleece::JSONEncoder> _in;                   // Accumulates body data (not JSON)
            uint32_t                             _propertiesSize{0};    // Length of properties in bytes
            fleece::slice_ostream                _propertiesRemaining;  // Subrange of _properties still to be read
            uint32_t                             _unackedBytes{0};      // # bytes received that haven't been ACKed yet
            alloc_slice                          _properties;           // Just the (still encoded) properties
            alloc_slice                          _body;                 // Just the body
            alloc_slice                          _bodyAsFleece;         // Body re-encoded into Fleece [lazy]
            const MessageSize                    _outgoingSize{0};
            bool                                 _complete{false};
            bool                                 _responded{false};
        };

}}  // namespace litecore::blip
