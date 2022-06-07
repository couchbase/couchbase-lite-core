//
// MessageBuilder.hh
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
#include "Message.hh"
#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>
#include <sstream>

namespace litecore { namespace blip {
    class BuiltMessage;


    /** A callback to provide data for an outgoing message. When called, it should copy data
        to the location in the `buf` parameter, with a maximum length of `capacity`. It should
        return the number of bytes written, or 0 on EOF, or a negative number on error. */
    class IMessageDataSource {
    public:
        virtual int operator() (void *buf, size_t capacity) =0;
        virtual ~IMessageDataSource() = default;
    };

    using MessageDataSource = std::shared_ptr<IMessageDataSource>;


    /** A temporary object used to construct an outgoing message (request or response).
        The message is sent by calling Connection::sendRequest() or MessageIn::respond(). */
    class MessageBuilder {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        typedef std::pair<slice, slice> property;

        /** Constructs a MessageBuilder for a request, optionally setting its Profile property. */
        explicit MessageBuilder(slice profile = fleece::nullslice);

        /** Constructs a MessageBuilder for a request, with a list of properties. */
        explicit MessageBuilder(std::initializer_list<property>);

        /** Constructs a MessageBuilder for a response. */
        explicit MessageBuilder(MessageIn *inReplyTo);

        bool isResponse() const                     {return type != kRequestType;}

        void setProfile(slice profile);

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
        fleece::JSONEncoder& jsonBody()          {finishProperties(); return _out;}

        /** Adds data to the body of the message. No more properties can be added afterwards. */
        MessageBuilder& write(slice s);
        MessageBuilder& operator<< (slice s)        {return write(s);}

        /** Clears the MessageBuilder so it can be used to create another message. */
        void reset();

        /** Callback to provide the body of the message; will be called whenever data is needed. */
        MessageDataSource dataSource;

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
        friend class BuiltMessage;

        FrameFlags flags() const;
        alloc_slice finish();
        void writeTokenizedString(std::ostream &out, slice str);

        MessageType type {kRequestType};

    private:
        void finishProperties();

        fleece::JSONEncoder _out;    // Actually using it for the entire msg, not just JSON
        std::stringstream _properties;  // Accumulates encoded properties
        bool _wroteProperties {false};  // Have _properties been written to _out yet?
    };


    /** Intermediate value produced by a MessageBuilder, to be passed to the Connection.
        (Unlike MessageBuilder this class is copyable, so instances can be captured by
        `std::function`. That makes it useable by async code.) */
    class BuiltMessage {
    public:
        BuiltMessage(MessageBuilder&);

        MessageDataSource       dataSource;
        MessageProgressCallback onProgress;

    protected:
        friend class MessageOut;

        FrameFlags          _flags;
        fleece::alloc_slice _payload;
    };

} }
