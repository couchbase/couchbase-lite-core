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
#include "FleeceCpp.hh"
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


    // How many bytes to receive before sending an ACK
    static const size_t kIncomingAckThreshold = 50000;


#pragma mark - MESSAGE BUILDER:

    
    MessageBuilder::MessageBuilder(slice profile)
    {
        if (profile)
            addProperty("Profile"_sl, profile);
    }


    MessageBuilder::MessageBuilder(MessageIn *inReplyTo)
    :MessageBuilder()
    {
        assert(!inReplyTo->isResponse());
        assert(!inReplyTo->noReply());
        type = kResponseType;
        urgent = inReplyTo->urgent();
    }


    MessageBuilder::MessageBuilder(initializer_list<property> properties)
    :MessageBuilder()
    {
        addProperties(properties);
    }


    MessageBuilder& MessageBuilder::addProperties(initializer_list<property> properties) {
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
    static void writeTokenizedString(ostream &out, slice str) {
        assert(str.findByte('\0') == nullptr);
        assert(str.size == 0  || str[0] >= 32);
        for (slice *special = &kSpecialProperties[0]; *special; ++special) {
            if (str == *special) {
                uint8_t token[2] = {(uint8_t)(special - kSpecialProperties + 1), 0};
                out.write((char*)&token, 2);
                return;
            }
        }
        out.write((char*)str.buf, str.size);
        out << '\0';
    }


    MessageBuilder& MessageBuilder::addProperty(slice name, slice value) {
        assert(!_wroteProperties);
        writeTokenizedString(_properties, name);
        writeTokenizedString(_properties, value);
        return *this;
    }


    MessageBuilder& MessageBuilder::addProperty(slice name, int64_t value) {
        char valueStr[30];
        return addProperty(name, slice(valueStr, sprintf(valueStr, "%lld", value)));
    }


    void MessageBuilder::finishProperties() {
        if (!_wroteProperties) {
            string properties = _properties.str();
            _properties.clear();
            size_t propertiesSize = properties.size();
            char buf[kMaxVarintLen64];
            slice encodedSize(buf, PutUVarInt(buf, propertiesSize));
            _out.writeRaw(encodedSize);
            _out.writeRaw(slice(properties));
            _wroteProperties = true;
        }
    }


    MessageBuilder& MessageBuilder::write(slice data) {
        if(!_wroteProperties)
            finishProperties();
        _out.writeRaw(data);
        return *this;
    }


    alloc_slice MessageBuilder::extractOutput() {
        finishProperties();
        return _out.finish();
    }


    void MessageBuilder::reset() {
        _out.reset();
        _properties.clear();
        _wroteProperties = false;
    }


#pragma mark - MESSAGE OUT:


    MessageOut::MessageOut(Connection *connection,
                           FrameFlags flags,
                           alloc_slice payload,
                           MessageNo number)
    :Message(flags, number)
    ,_connection(connection)
    ,_payload(payload)
    {
        assert(payload.size < 1ull<<32);
        assert(!(_flags & kCompressed));    //TODO: Implement compression

        if (type() == kRequestType && !noReply()) {
            // The MessageIn's flags will be updated when the 1st frame of the response arrives;
            // the type might become kErrorType, and kUrgent or kCompressed might be set.
            _pendingResponse = new MessageIn(_connection, (FrameFlags)kResponseType, _number);
        }
    }


    slice MessageOut::nextFrameToSend(size_t maxSize, FrameFlags &outFlags) {
        size_t size = min(maxSize, _payload.size - _bytesSent);
        slice frame = _payload(_bytesSent, size);
        _bytesSent += size;
        _unackedBytes += size;
        outFlags = flags();
        if (_bytesSent < _payload.size)
            outFlags = (FrameFlags)(outFlags | kMoreComing);
        return frame;
    }


    void MessageOut::receivedAck(uint32_t byteCount) {
        if (byteCount <= _bytesSent)
            _unackedBytes = min(_unackedBytes, (uint32_t)(_bytesSent - byteCount));
    }


    Retained<MessageIn> MessageOut::detachResponse() {
        if (_pendingResponse)
            _pendingResponse->_number = _number;
        return move(_pendingResponse);
    }


    FutureResponse MessageOut::futureResponse() {
        auto response = _pendingResponse;
        return response ? response->createFutureResponse() : FutureResponse{};
    }


#pragma mark - MESSAGE IN:


    MessageIn::MessageIn(Connection *connection, FrameFlags flags, MessageNo n)
    :Message(flags, n)
    ,_connection(connection)
    { }


    FutureResponse MessageIn::createFutureResponse() {
        assert(!_future);
        _future = new Future<Retained<MessageIn>>;
        return _future;
    }


    bool MessageIn::receivedFrame(slice frame, FrameFlags frameFlags) {
        size_t bytesReceived = frame.size;
        if (_in) {
            bytesReceived += _in->length();
        } else {
            // On first frame, update my flags and allocate the Writer:
            assert(_number > 0);
            LogTo(BLIPLog, "Receiving %s #%llu, flags=%02x",
                  kMessageTypeNames[type()], _number, flags());
            _flags = frameFlags;
            if (_flags & kCompressed)
                throw "compression isn't supported yet";  //TODO: Implement compression
            _in.reset(new Writer);
            // Get the length of the properties, and move `frame` past the length field:
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

        _unackedBytes += frame.size;
        if (_unackedBytes >= kIncomingAckThreshold) {
            // Send an ACK every 50k bytes:
            MessageType msgType = isResponse() ? kAckResponseType : kAckRequestType;
            uint8_t buf[kMaxVarintLen64];
            alloc_slice payload(buf, PutUVarInt(buf, bytesReceived));
            Retained<MessageOut> ack = new MessageOut(_connection,
                                                      (FrameFlags)(msgType | kUrgent | kNoReply),
                                                      payload,
                                                      _number);
            _connection->send(ack);
            _unackedBytes = 0;
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

            LogTo(BLIPLog, "Finished receiving %s #%llu, flags=%02x",
                  kMessageTypeNames[type()], _number, flags());
            if (_future) {
                _future->fulfil(this);
                _future = nullptr;
            }
            return true;
        }
    }


    slice MessageIn::errorDomain() const {
        if (!isError())
            return nullslice;
        return property("Error-Domain"_sl);
    }


    int MessageIn::errorCode() const {
        if (!isError())
            return 0;
        return (int) intProperty("Error-Code"_sl);
    }


    void MessageIn::respond(MessageBuilder &mb) {
        if (noReply()) {
            WarnError("Attempted to respond to noReply message");
            return;
        }
        if (mb.type == kRequestType)
            mb.type = kResponseType;
        Retained<MessageOut> message = new MessageOut(_connection, mb, _number);
        _connection->send(message);
    }


    void MessageIn::respondWithError(slice domain, int code, slice message) {
        if (!noReply()) {
            MessageBuilder mb(this);
            mb.makeError(domain, code, message);
            respond(mb);
        }
    }


    void MessageIn::notHandled() {
        respondWithError("BLIP"_sl, 404);
    }


    slice MessageIn::property(slice property) const {
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


    long MessageIn::intProperty(slice name, long defaultValue) const {
        string value = property(name).asString();
        if (value.empty())
            return defaultValue;
        char *end;
        long result = strtol(value.c_str(), &end, 10);
        if (*end != '\0')
            return defaultValue;
        return result;
    }


    fleeceapi::Value MessageIn::JSONBody() {
        if (!_bodyAsFleece)
            _bodyAsFleece = FLData_ConvertJSON({_body.buf, _body.size}, nullptr);
        return fleeceapi::Value::fromData(_bodyAsFleece);
    }


} }
