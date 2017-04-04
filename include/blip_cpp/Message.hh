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
#include "Logging.hh"
#include "Fleece.h"
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_map>

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
        {/*Log("NEW Message<%p, %s #%llu>", this, typeName(), _number);*/}

        FrameFlags flags() const            {return _flags;}
        bool hasFlag(FrameFlags f) const    {return (_flags & f) != 0;}
        bool isAck() const                  {return type() == kAckRequestType ||
                                                    type() == kAckResponseType;}
        MessageType type() const            {return (MessageType)(_flags & kTypeMask);}
        const char* typeName() const        {return kMessageTypeNames[type()];}

        void sendProgress(MessageProgress::State state,
                          MessageSize bytesSent, MessageSize bytesReceived,
                          MessageIn *reply);

//        ~Message()    {Log("DELETE Message<%p, %s #%llu>",
//                           this, typeName(), _number);}

        FrameFlags _flags;
        MessageNo _number;
        MessageProgressCallback _onProgress;
    };


    /** An incoming message. */
    class MessageIn : public Message {
    public:
        /** The body of the message. */
        alloc_slice body() const            {return _body;}

        fleeceapi::Value JSONBody();

        /** Gets a property value */
        slice property(slice property) const;
        long intProperty(slice property, long defaultValue =0) const;
        bool boolProperty(slice property, bool defaultValue =false) const;

        /** Returns information about an error (if this message is an error.) */
        Error getError() const;

        /** Sends a response. */
        void respond(MessageBuilder&);

        /** Sends an error as a response. */
        void respondWithError(Error);

        /** Responds with an error saying that the message went unhandled.
            Call this if you don't know what to do with a request. */
        void notHandled();

    protected:
        friend class MessageOut;
        friend class BLIPIO;

        MessageIn(Connection*, FrameFlags, MessageNo, MessageProgressCallback =nullptr, MessageSize outgoingSize =0);
        virtual ~MessageIn();
        bool receivedFrame(slice, FrameFlags);

    private:
        Connection* const _connection;          // The owning BLIP connection
        std::unique_ptr<fleeceapi::JSONEncoder> _in; // Accumulates message data (not just JSON)
        std::unique_ptr<zlibcomplete::GZipDecompressor> _decompressor;
        uint32_t _propertiesSize {0};           // Length of properties in bytes
        uint32_t _unackedBytes {0};             // # bytes received that haven't been ACKed yet
        alloc_slice _properties;                // Just the (still encoded) properties
        alloc_slice _body;                      // Just the body
        alloc_slice _bodyAsFleece;              // Body re-encoded into Fleece [lazy]
        const MessageSize _outgoingSize;
    };


    /** A temporary object used to construct an outgoing message (request or response).
        The message is sent by calling Connection::sendRequest() or MessageIn::respond(). */
    class MessageBuilder {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        typedef std::pair<slice, slice> property;

        /** Constructs a MessageBuilder for a request, optionally setting its Profile property. */
        MessageBuilder(slice profile = fleece::nullslice);

        /** Constructs a MessageBuilder for a request, with a list of properties. */
        MessageBuilder(std::initializer_list<property>);

        /** Constructs a MessageBuilder for a response. */
        MessageBuilder(MessageIn *inReplyTo);

        /** Adds a property. */
        MessageBuilder& addProperty(slice name, slice value);

        /** Adds a property with an integer value. */
        MessageBuilder& addProperty(slice name, int64_t value);

        /** Adds multiple properties. */
        MessageBuilder& addProperties(std::initializer_list<property>);

        struct propertySetter {
            MessageBuilder &builder;
            slice name;
            MessageBuilder& operator= (slice value)   {return builder.addProperty(name, value);}
            MessageBuilder& operator= (int64_t value) {return builder.addProperty(name, value);}
        };
        propertySetter operator[] (slice name)        { return {*this, name}; }

        /** Makes a response an error. */
        void makeError(Error);

        /** JSON encoder that can be used to write JSON to the body. */
        fleeceapi::JSONEncoder& jsonBody()          {finishProperties(); return _out;}

        /** Adds data to the body of the message. No more properties can be added afterwards. */
        MessageBuilder& write(slice s);
        MessageBuilder& operator<< (slice s)        {return write(s);}

        /** Clears the MessageBuilder so it can be used to create another message. */
        void reset();

        /** Callback to be invoked as the message is delivered (and replied to, if appropriate) */
        MessageProgressCallback onProgress;

        /** Is the message urgent (will be sent more quickly)? */
        bool urgent         {false};

        /** Should the message's body be gzipped? */
        bool compressed     {false};

        /** Should the message refuse replies? */
        bool noreply        {false};

    protected:
        friend class MessageIn;
        friend class MessageOut;

        FrameFlags flags() const;
        alloc_slice extractOutput();

        MessageType type {kRequestType};

    private:
        void finishProperties();

        fleeceapi::JSONEncoder _out;    // Actually using it for the entire msg, not just JSON
        std::stringstream _properties;  // Accumulates encoded properties
        bool _wroteProperties {false};  // Have _properties been written to _out yet?
        uint32_t _propertiesLength {0};
    };

} }
