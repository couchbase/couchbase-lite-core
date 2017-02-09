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
#include "Writer.hh"
#include "Future.hh"
#include "Logging.hh"
#include <functional>
#include <memory>
#include <unordered_map>

namespace litecore { namespace blip {

    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;

    class Connection;
    class MessageBuilder;
    class MessageIn;


    /** Abstract base class of messages */
    class Message : public RefCounted {
    public:
        bool isResponse() const             {return type() >= kResponseType;}
        bool isError() const                {return type() == kErrorType;}
        bool urgent() const                 {return hasFlag(kUrgent);}
        bool noReply() const                {return hasFlag(kNoReply);}

        MessageNo number() const            {return _number;}

    protected:
        Message(FrameFlags f, MessageNo n)  :_flags(f), _number(n)
                            {/*Log("NEW Message<%p, %s #%llu>", this, typeName(), _number);*/}
        FrameFlags flags() const            {return _flags;}
        bool hasFlag(FrameFlags f) const    {return (_flags & f) != 0;}
        bool isAck() const                  {return type() == kAckRequestType ||
                                                    type() == kAckResponseType;}
        MessageType type() const            {return (MessageType)(_flags & kTypeMask);}
        const char* typeName() const        {return kMessageTypeNames[type()];}

//        ~Message()    {Log("DELETE Message<%p, %s #%llu>",
//                           this, typeName(), _number);}

        FrameFlags _flags;
        MessageNo _number;
    };


    /** A Future that will resolve to a MessageIn. */
    typedef Retained<Future<Retained<MessageIn>>> FutureResponse;


    /** An incoming message. */
    class MessageIn : public Message {
    public:
        /** The body of the message. */
        alloc_slice body() const            {return _body;}

        /** Gets a property value */
        slice property(slice property) const;
        long intProperty(slice property, long defaultValue =0) const;

        /** The error domain (if this message is an error.) */
        slice errorDomain() const;

         /** The error code (if this message is an error.) */
         int errorCode() const;

        /** Sends a response. */
        void respond(MessageBuilder&);

        /** Sends an error as a response. */
        void respondWithError(slice domain, int code, slice message);

    protected:
        friend class MessageOut;
        friend class BLIPIO;

        MessageIn(Connection*, FrameFlags, MessageNo);
        bool receivedFrame(slice, FrameFlags);
        void messageComplete();
        FutureResponse createFutureResponse();

    private:
        Connection* const _connection;
        std::unique_ptr<fleece::Writer> _in;
        uint32_t _propertiesSize {0};
        uint32_t _unackedBytes {0};
        alloc_slice _properties;
        alloc_slice _body;
        FutureResponse _future;
    };


    /** A temporary object used to construct an outgoing message (request or response).
        The message is sent by calling Connection::sendRequest() or MessageIn::respond(). */
    class MessageBuilder {
    public:
        typedef std::pair<slice, slice> property;

        /** Constructs a MessageBuilder for a request. */
        MessageBuilder();

        /** Constructs a MessageBuilder for a request, with a list of properties. */
        MessageBuilder(std::initializer_list<property>);

        /** Constructs a MessageBuilder for a response. */
        MessageBuilder(MessageIn *inReplyTo);

        /** Adds a property. */
        MessageBuilder& addProperty(slice name, slice value);

        /** Adds a property with an integer value. */
        MessageBuilder& addProperty(slice name, int value);

        /** Adds multiple properties. */
        MessageBuilder& addProperties(std::initializer_list<property>);

        /** Makes a response an error. */
        void makeError(slice domain, int code, slice message);

        /** Adds data to the body of the message. No more properties can be added afterwards. */
        MessageBuilder& write(slice);
        MessageBuilder& operator<< (slice s)  {return write(s);}

        /** Clears the MessageBuilder so it can be used to create another message. */
        void reset();

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

        MessageType type    {kRequestType};

    private:
        void finishProperties();

        fleece::Writer _out;
        const void *_propertiesSizePos;
    };

} }
