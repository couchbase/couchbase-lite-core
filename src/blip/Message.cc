//
//  Message.cc
//  LiteCore
//
//  Created by Jens Alfke on 1/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Message.hh"
#include "BLIPConnection.hh"
#include "BLIPInternal.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {


#pragma mark - MESSAGE BUILDER:


    // Property names/values that are encoded as single bytes (first is Ctrl-A, etc.)
    // Protocol v2.0. CHANGING THIS ARRAY WILL BREAK BLIP PROTOCOL COMPATIBILITY!!
    static slice kSpecialProperties[] = {
        "Profile"_sl,
        "Error-Code"_sl,
        "Error-Domain"_sl,

        "Content-Type"_sl,
        "application/json"_sl,
        "application/octet-stream"_sl,
        "text/plain; charset=UTF-8"_sl,
        "text/xml"_sl,

        "Accept"_sl,
        "Cache-Control"_sl,
        "must-revalidate"_sl,
        "If-Match"_sl,
        "If-None-Match"_sl,
        "Location"_sl,
        nullslice
    };


    static const size_t kPropertiesSizeReserved = 1;


    MessageBuilder::MessageBuilder()
    {
        _propertiesSizePos = _out.reserveSpace(kPropertiesSizeReserved);
    }


    MessageBuilder::MessageBuilder(MessageIn *inReplyTo)
    :MessageBuilder()
    {
        type = kResponseType;
        urgent = inReplyTo->urgent();
    }


    MessageBuilder::MessageBuilder(std::initializer_list<MessageBuilder::property> properties)
    :MessageBuilder()
    {
        addProperties(properties);
    }


    MessageBuilder& MessageBuilder::addProperties(std::initializer_list<MessageBuilder::property> properties) {
        for (const property &p : properties)
            addProperty(p.first, p.second);
        return *this;
    }


    void MessageBuilder::makeError(slice domain, int code, slice message) {
        assert(domain);
        type = kErrorType;
        addProperty("Error-Domain"_sl, domain);
        addProperty("Error-Code"_sl, code);
        if (message)
            addProperty("Error-Message"_sl, message);
    }


    FrameFlags MessageBuilder::flags() const {
        int flags = type & kTypeMask;
        if (urgent)     flags |= kUrgent;
        if (compressed) flags |= kCompressed;
        if (noreply)    flags |= kNoReply;
        return (FrameFlags)flags;
    }


    // Abbreviates certain special strings as a single byte
    static slice tokenize(slice str, uint8_t &tokenBuf) {
        for (slice *special = &kSpecialProperties[0]; *special; ++special) {
            if (str == *special) {
                tokenBuf = (uint8_t)(special - kSpecialProperties + 1);
                return {&tokenBuf, 1};
            }
        }
        return str;
    }


    MessageBuilder& MessageBuilder::addProperty(slice name, slice value) {
        assert(name.findByte('\0') == nullptr);
        assert(value.findByte('\0') == nullptr);
        assert(name.size == 0  || name[0] >= 32);
        assert(value.size == 0 || value[0] >= 32);

        assert(_propertiesSizePos != nullptr);      // already finished properties

        uint8_t nameToken, valueToken;
        _out << tokenize(name, nameToken) << '\0' << tokenize(value, valueToken) << '\0';
        return *this;
    }


    MessageBuilder& MessageBuilder::addProperty(slice name, int value) {
        char valueStr[20];
        return addProperty(name, slice(valueStr, sprintf(valueStr, "%d", value)));
    }


    void MessageBuilder::finishProperties() {
        if (_propertiesSizePos) {
            size_t propertiesSize = _out.length() - kPropertiesSizeReserved;
            char buf[kMaxVarintLen64];
            slice encodedSize(buf, PutUVarInt(buf, propertiesSize));
            if (encodedSize.size == 1) {
                // Overwrite the size placeholder with the real size byte:
                _out.rewrite(_propertiesSizePos, encodedSize);
            } else {
                // Oh crap, the properties size field requires 2+ bytes. Gotta start over:
                auto copiedProps = _out.extractOutput();
                _out.reset();
                _out.write(encodedSize);
                _out.write(copiedProps);
            }
            _propertiesSizePos = nullptr;
        }
    }


    MessageBuilder& MessageBuilder::write(slice data) {
        if(_propertiesSizePos)
            finishProperties();
        _out.write(data);
        return *this;
    }


    alloc_slice MessageBuilder::extractOutput() {
        finishProperties();
        return _out.extractOutput();
    }


    void MessageBuilder::reset() {
        _out.reset();
        _propertiesSizePos = _out.reserveSpace(sizeof(uint16_t));
    }


#pragma mark - MESSAGE OUT:


    MessageOut::MessageOut(Connection *connection, MessageBuilder &builder, MessageNo number)
    :Message(builder.flags())
    ,_connection(connection)
    ,_payload(builder.extractOutput())
    {
        _number = number;
        assert(!(_flags & kCompressed));    //TODO: Implement compression
    }


    slice MessageOut::nextFrameToSend(size_t maxSize, FrameFlags &outFlags) {
        size_t size = min(maxSize, _payload.size - _bytesSent);
        slice frame = _payload(_bytesSent, size);
        _bytesSent += size;
        outFlags = flags();
        if (_bytesSent < _payload.size)
            outFlags = (FrameFlags)(outFlags | kMoreComing);
        return frame;
    }


    MessageIn* MessageOut::pendingResponse() {
        if (!_pendingResponse) {
            if (type() == kRequestType && !noReply()) {
                // The flags will be updated when the first frame of the response arrives;
                // the type might become kErrorType, and kUrgent or kCompressed might be set.
                _pendingResponse = new MessageIn(_connection, (FrameFlags)kResponseType, _number);
            }
        }
        return _pendingResponse;
    }


#pragma mark - MESSAGE IN:


    MessageIn::MessageIn(Connection *connection, FrameFlags flags, MessageNo n)
    :Message(flags)
    ,_connection(connection)
    {
        assert(n > 0);
        _number = n;
    }


    bool MessageIn::receivedFrame(slice frame, FrameFlags frameFlags) {
        if (!_in) {
            // On first frame, update my flags and allocate the Writer:
            _flags = frameFlags;
            if (_flags & kCompressed)
                throw "compression isn't supported yet";  //TODO: Implement compression
            _in.reset(new Writer);
            // Get the length of the properties:
            if (!ReadUVarInt32(&frame, &_propertiesSize))
                throw "frame too small";
        }

        if (!_properties && (_in->length() + frame.size) >= _propertiesSize) {
            // OK, we now have the complete properties:
            size_t remaining = _propertiesSize - _in->length();
            _in->write(frame.buf, remaining);
            frame.moveStart(remaining);
            _properties = _in->extractOutput();
            if (_properties.size > 0 && _properties[_properties.size - 1] != 0)
                throw "message properties not null-terminated";
            _in->reset();
        }

        _in->write(frame);

        if (frameFlags & kMoreComing) {
            return false;
        } else {
            // Completed!
            if (!_properties)
                throw "message ends before end of properties";
            _body = _in->extractOutput();
            _in.reset();

            messageComplete();
            return true;
        }
    }


    void MessageIn::messageComplete() {
        if (onComplete)
            onComplete(this);
        if (type() == kRequestType)
            _connection->delegate().onRequestReceived(this);
        else
            _connection->delegate().onResponseReceived(this);
    }


    void MessageIn::respond(MessageBuilder &mb) {
        assert(!noReply());
        if (mb.type == kRequestType)
            mb.type = kResponseType;
        Retained<MessageOut> message = new MessageOut(_connection, mb, _number);
        _connection->send(message);
    }


    void MessageIn::respondWithError(slice domain, int code, slice message) {
        MessageBuilder mb(this);
        mb.makeError(domain, code, message);
        respond(mb);
    }


    slice MessageIn::operator[] (slice property) const {
        // Note: using strlen here is safe. It can't fall off the end of _properties, because the
        // receivedFrame() method has already verified that _properties ends with a zero byte.
        // OPT: This lookup isn't very efficient. If it turns out to be a hot-spot, we could cache
        // the starting point of every property string.
        auto key = (const char*)_properties.buf;
        auto end = (const char*)_properties.end();
        while (key < end) {
            auto endOfKey = key + strlen(key);
            auto val = endOfKey + 1;
            if (val >= end)
                break;  // illegal: missing value
            auto endOfVal = val + strlen(val);
            if (property == slice(key, endOfKey))
                return slice(val, endOfVal);
            key = endOfVal + 1;
        }
        return nullslice;
    }


} }
