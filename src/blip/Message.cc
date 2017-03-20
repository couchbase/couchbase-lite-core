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
#include <zlc/zlibcomplete.hpp>
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


    void Message::sendProgress(MessageProgress::State state,
                               MessageSize bytesSent, MessageSize bytesReceived,
                               MessageIn *reply) {
        if (_onProgress)
            _onProgress({state, bytesSent, bytesReceived, reply});
    }


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
        return addProperty(name, slice(valueStr, sprintf(valueStr, "%lld", (long long)value)));
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
            _propertiesLength = (uint32_t)_out.bytesWritten();
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
        alloc_slice output = _out.finish();

        if (compressed) {
            compressed = false;
            if (output.size > _propertiesLength) {
                // Compress the body (but not the properties):      //OPT: Could be optimized
                slice body = output;
                body.moveStart(_propertiesLength);
                zlibcomplete::GZipCompressor compressor;
                string zip1 = compressor.compress(body.asString());
                string zip2 = compressor.finish();
                size_t len1 = zip1.size(), len2 = zip2.size();
                if (len1 + len2 < (output.size - _propertiesLength)) {
                    LogToAt(BLIPLog, Debug, "Message compressed from %zu to %zu bytes",
                            output.size, _propertiesLength + len1 + len2);
                    memcpy((void*)&output[_propertiesLength],        zip1.data(), len1);
                    memcpy((void*)&output[_propertiesLength + len1], zip2.data(), len2);
                    output.size = _propertiesLength + len1 + len2;
                    compressed = true;
                }
            }
        }
        return output;
    }


    void MessageBuilder::reset() {
        onProgress = nullptr;
        urgent = compressed = noreply = false;
        _out.reset();
        _properties.clear();
        _wroteProperties = false;
        _propertiesLength = 0;
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
        assert(payload.size <= UINT32_MAX);
    }


    slice MessageOut::nextFrameToSend(size_t maxSize, FrameFlags &outFlags) {
        size_t size = min(maxSize, _payload.size - _bytesSent);
        slice frame = _payload(_bytesSent, size);
        _bytesSent += size;
        _unackedBytes += size;
        outFlags = flags();
        MessageProgress::State state;
        if (_bytesSent < _payload.size) {
            outFlags = (FrameFlags)(outFlags | kMoreComing);
            state = MessageProgress::kSending;
        } else if (noReply()) {
            state = MessageProgress::kComplete;
        } else {
            state = MessageProgress::kAwaitingReply;
        }
        sendProgress(state, _bytesSent, 0, nullptr);
        return frame;
    }


    void MessageOut::receivedAck(uint32_t byteCount) {
        if (byteCount <= _bytesSent)
            _unackedBytes = min(_unackedBytes, (uint32_t)(_bytesSent - byteCount));
    }


    MessageIn* MessageOut::createResponse() {
        if (type() != kRequestType || noReply())
            return nullptr;
        // Note: The MessageIn's flags will be updated when the 1st frame of the response arrives;
        // the type might become kErrorType, and kUrgent or kCompressed might be set.
        return new MessageIn(_connection, (FrameFlags)kResponseType, _number,
                             _onProgress, _payload.size);
    }


#pragma mark - MESSAGE IN:


    MessageIn::~MessageIn()
    {
        
    }

    MessageIn::MessageIn(Connection *connection, FrameFlags flags, MessageNo n,
                         MessageProgressCallback onProgress, MessageSize outgoingSize)
    :Message(flags, n)
    ,_connection(connection)
    ,_outgoingSize(outgoingSize)
    {
        _onProgress = onProgress;
    }


    bool MessageIn::receivedFrame(slice frame, FrameFlags frameFlags) {
        MessageSize bytesReceived = frame.size;
        if (_in) {
            bytesReceived += _in->bytesWritten();
        } else {
            // On first frame, update my flags and allocate the Writer:
            assert(_number > 0);
            _flags = (FrameFlags)(frameFlags & ~kMoreComing);
            _connection->log("Receiving %s #%llu, flags=%02x",
                             kMessageTypeNames[type()], _number, flags());
            _in.reset(new fleeceapi::JSONEncoder);
            // Get the length of the properties, and move `frame` past the length field:
            if (!ReadUVarInt32(&frame, &_propertiesSize))
                throw "frame too small";
        }

        if (!_properties && (_in->bytesWritten() + frame.size) >= _propertiesSize) {
            // OK, we now have the complete properties:
            size_t remaining = _propertiesSize - _in->bytesWritten();
            _in->writeRaw({frame.buf, remaining});
            frame.moveStart(remaining);
            _properties = _in->finish();
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

        if (_properties && (_flags & kCompressed)) {
            if (!_decompressor)
                _decompressor.reset( new zlibcomplete::GZipDecompressor );
            string output = _decompressor->decompress(frame.asString());
            if (output.empty())
                throw "invalid gzipped data";
            _in->writeRaw(slice(output));
        } else {
            _in->writeRaw(frame);
        }

        if (frameFlags & kMoreComing) {
            sendProgress(MessageProgress::kReceivingReply, _outgoingSize, bytesReceived, nullptr);
            return false;
        } else {
            // Completed!
            if (!_properties)
                throw "message ends before end of properties";
            _body = _in->finish();
            _in.reset();
            _decompressor.reset();

            _connection->log("Finished receiving %s #%llu, flags=%02x",
                             kMessageTypeNames[type()], _number, flags());
            sendProgress(MessageProgress::kComplete, _outgoingSize, bytesReceived, this);
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
            _connection->log("Ignoring attempt to respond to a noReply message");
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
        uint8_t specbuf[1];
        for (uint8_t i = 0; kSpecialProperties[i]; ++i) {
            if (property == kSpecialProperties[i]) {
                specbuf[0] = i + 1;
                property = slice(&specbuf, 1);
                break;
            }
        }

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


    bool MessageIn::boolProperty(slice name, bool defaultValue) const {
        slice value = property(name);
        if (value.caseEquivalent("true"_sl) || value.caseEquivalent("YES"_sl))
            return true;
        else if (value.caseEquivalent("false"_sl) || value.caseEquivalent("NO"_sl))
            return false;
        else
            return intProperty(name, defaultValue) != 0;
    }


    fleeceapi::Value MessageIn::JSONBody() {
        if (!_bodyAsFleece)
            _bodyAsFleece = FLData_ConvertJSON({_body.buf, _body.size}, nullptr);
        return fleeceapi::Value::fromData(_bodyAsFleece);
    }


} }
