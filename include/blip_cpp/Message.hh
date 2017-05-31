//
//  Message.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "BLIPProtocol.hh"
#include "RefCounted.hh"
#include "Fleece.h"
#include "slice.hh"
#include <functional>
#include <memory>
#include <mutex>

namespace fleeceapi {
    class Value;
}
namespace zlibcomplete {
    class GZipDecompressor;
    class GZipCompressor;
}

namespace litecore { namespace blip {

    class Connection;
    class MessageBuilder;
    class MessageIn;


    /** Progress notification for an outgoing request. */
    struct MessageProgress {
        enum State {
            kQueued,
            kSending,
            kAwaitingReply,
            kReceivingReply,
            kComplete
        } state;
        MessageSize bytesSent;
        MessageSize bytesReceived;
        Retained<MessageIn> reply;
    };

    using MessageProgressCallback = std::function<void(const MessageProgress&)>;


    struct Error {
        const fleece::slice domain;
        const int code {0};
        const fleece::slice message;

        Error()  { }
        Error(fleece::slice domain_, int code_, fleece::slice msg =fleece::nullslice)
        :domain(domain_), code(code_), message(msg)
        { }
    };

    // Like Error but with an allocated message string
    struct ErrorBuf : public Error {
        ErrorBuf()  { }

        ErrorBuf(fleece::slice domain, int code, fleece::alloc_slice msg)
        :Error(domain, code, msg)
        ,_messageBuf(msg)
        { }

    private:
        const fleece::alloc_slice _messageBuf;
    };


    /** Abstract base class of messages */
    class Message : public RefCounted {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        
        bool isResponse() const             {return type() >= kResponseType;}
        bool isError() const                {return type() == kErrorType;}
        bool urgent() const                 {return hasFlag(kUrgent);}
        bool noReply() const                {return hasFlag(kNoReply);}

        MessageNo number() const            {return _number;}

    protected:
        Message(FrameFlags f, MessageNo n)
        :_flags(f), _number(n)
        {
            /*Log("NEW Message<%p, %s #%llu>", this, typeName(), _number);*/
        }

        virtual ~Message() {
            //Log("DELETE Message<%p, %s #%llu>", this, typeName(), _number);
        }

        FrameFlags flags() const            {return _flags;}
        bool hasFlag(FrameFlags f) const    {return (_flags & f) != 0;}
        bool isAck() const                  {return type() == kAckRequestType ||
                                                    type() == kAckResponseType;}
        MessageType type() const            {return (MessageType)(_flags & kTypeMask);}
        const char* typeName() const        {return kMessageTypeNames[type()];}

        void sendProgress(MessageProgress::State state,
                          MessageSize bytesSent, MessageSize bytesReceived,
                          MessageIn *reply);


        FrameFlags _flags;
        MessageNo _number;
        MessageProgressCallback _onProgress;
    };


    /** An incoming message. */
    class MessageIn : public Message {
    public:
        /** Gets a property value */
        slice property(slice property) const;
        long intProperty(slice property, long defaultValue =0) const;
        bool boolProperty(slice property, bool defaultValue =false) const;

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
        fleeceapi::Value JSONBody();

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

    protected:
        friend class MessageOut;
        friend class BLIPIO;

        enum ReceiveState {
            kOther,
            kBeginning,
            kEnd
        };

        MessageIn(Connection*, FrameFlags, MessageNo, MessageProgressCallback =nullptr, MessageSize outgoingSize =0);
        virtual ~MessageIn();
        ReceiveState receivedFrame(slice, FrameFlags);

    private:
        Connection* const _connection;          // The owning BLIP connection
        std::mutex _receiveMutex;
        std::unique_ptr<fleeceapi::JSONEncoder> _in; // Accumulates message data (not just JSON)
        std::unique_ptr<zlibcomplete::GZipDecompressor> _decompressor;
        uint32_t _propertiesSize {0};           // Length of properties in bytes
        uint32_t _unackedBytes {0};             // # bytes received that haven't been ACKed yet
        alloc_slice _properties;                // Just the (still encoded) properties
        alloc_slice _body;                      // Just the body
        alloc_slice _bodyAsFleece;              // Body re-encoded into Fleece [lazy]
        const MessageSize _outgoingSize {0};
        bool _complete {false};
    };

} }
