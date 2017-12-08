//
//  Message.cc
//  LiteCore
//
//  Created by Jens Alfke on 1/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Message.hh"
#include "MessageOut.hh"
#include "BLIPConnection.hh"
#include "BLIPInternal.hh"
#include "Codec.hh"
#include "FleeceCpp.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>
#include <sstream>

#include <iostream>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {


    // How many bytes to receive before sending an ACK
    static const size_t kIncomingAckThreshold = 50000;


    void Message::sendProgress(MessageProgress::State state,
                               MessageSize bytesSent, MessageSize bytesReceived,
                               MessageIn *reply) {
        if (_onProgress)
            _onProgress({state, bytesSent, bytesReceived, reply});
    }


    void Message::disconnected() {
        sendProgress(MessageProgress::kDisconnected, 0, 0, nullptr);
    }


    // Writes a slice to a stream. If it contains non-ASCII characters, it will be written as hex
    // inside "<<...>>". If empty, it's written as "<<>>".
    static ostream& dumpSlice(ostream& o, fleece::slice s) {
        if (s.size == 0)
            return o << "<<>>";
        auto buf = (const uint8_t*)s.buf;
        for (size_t i = 0; i < s.size; i++) {
            if (buf[i] < 32 || buf[i] > 126)
                return o << "<<" << s.hexString() << ">>";
        }
        return o << s;
    }


    void Message::dump(slice payload, slice body, std::ostream& out) {
        out << kMessageTypeNames[type()] << " #" << _number << ' ';
        if (_flags & kUrgent)  out << 'U';
        if (_flags & kNoReply)  out << 'N';
        if (_flags & kCompressed)  out << 'Z';
        out << " {";

        auto key = (const char*)payload.buf;
        auto end = (const char*)payload.end();
        while (key < end) {
            auto endOfKey = key + strlen(key);
            auto val = endOfKey + 1;
            if (val >= end)
                break;  // illegal: missing value
            auto endOfVal = val + strlen(val);

            slice propertyName = MessageBuilder::untokenizeProperty(slice(key, endOfKey));
            slice propertyValue= MessageBuilder::untokenizeProperty(slice(val, endOfVal));
            out << "\n\t";
            dumpSlice(out, propertyName);
            out << ": ";
            dumpSlice(out, propertyValue);
            key = endOfVal + 1;
        }
        if (body.size > 0) {
            out << "\n\tBODY: ";
            dumpSlice(out, body);
        }
        out << " }";
    }


#pragma mark - MESSAGEIN:
    

    MessageIn::~MessageIn()
    { }


    MessageIn::MessageIn(Connection *connection, FrameFlags flags, MessageNo n,
                         MessageProgressCallback onProgress, MessageSize outgoingSize)
    :Message(flags, n)
    ,_connection(connection)
    ,_outgoingSize(outgoingSize)
    {
        _onProgress = onProgress;
    }


    MessageIn::ReceiveState MessageIn::receivedFrame(Codec &codec,
                                                     slice frame,
                                                     FrameFlags frameFlags)
    {
        ReceiveState state = kOther;
        MessageSize bodyBytesReceived;
        {
            // First, lock the mutex:
            lock_guard<mutex> lock(_receiveMutex);

            // Update byte count and send acknowledgement packet when appropriate:
            _rawBytesReceived += frame.size;
            acknowledge(frame.size);

            auto mode = (frameFlags & kCompressed) ? Codec::Mode::SyncFlush : Codec::Mode::Raw;

            // Copy and remove the checksum from the end of the frame:
            uint8_t checksum[Codec::kChecksumSize];
            auto trailer = (void*)&frame[frame.size - Codec::kChecksumSize];
            memcpy(checksum, trailer, Codec::kChecksumSize);
            if (mode == Codec::Mode::SyncFlush) {
                // Replace checksum with the untransmitted deflate empty-block trailer,
                // which is conveniently the same size:
                static_assert(Codec::kChecksumSize == 4,
                              "Checksum not same size as deflate trailer");
                memcpy(trailer, "\x00\x00\xFF\xFF", 4);
            } else {
                // In uncompressed message, just trim off the checksum:
                frame.setSize(frame.size - Codec::kChecksumSize);
            }

            bool justFinishedProperties = false;
            if (!_in) {
                // First frame!
                // Update my flags and allocate the Writer:
                assert(_number > 0);
                _flags = (FrameFlags)(frameFlags & ~kMoreComing);
                _connection->logVerbose("Receiving %s #%llu, flags=%02x",
                                 kMessageTypeNames[type()], _number, flags());
                _in.reset(new fleeceapi::JSONEncoder);

                // Read just a few bytes to get the length of the properties (a varint at the
                // start of the frame):
                char buf[kMaxVarintLen32];
                slice dst(buf, sizeof(buf));
                codec.write(frame, dst, mode);
                dst = slice(buf, dst.buf);
                // Decode the properties length:
                if (!ReadUVarInt32(&dst, &_propertiesSize))
                    throw std::runtime_error("frame too small");
                if (_propertiesSize > kMaxPropertiesSize)
                    throw std::runtime_error("properties excessively large");
                // Allocate properties and put any remaining decoded data there:
                _properties = alloc_slice(_propertiesSize);
                _propertiesRemaining = _properties;
                _propertiesRemaining.writeFrom(dst.readAtMost(_propertiesSize));
                if (_propertiesRemaining.size == 0)
                    justFinishedProperties = true;
                // And anything left over after that becomes the start of the body:
                if (dst.size > 0)
                    _in->writeRaw(dst);
            }

            if (_propertiesRemaining.size > 0) {
                // Read into properties buffer:
                codec.write(frame, _propertiesRemaining, mode);
                if (_propertiesRemaining.size == 0)
                    justFinishedProperties = true;
            }
            if (justFinishedProperties) {
                // Finished reading properties:
                if (_propertiesSize > 0 && _properties[_propertiesSize - 1] != 0)
                    throw std::runtime_error("message properties not null-terminated");
#if DEBUG
                int nulls = 0;
                for (size_t i = 0; i < _propertiesSize; ++i)
                    if (_properties[i] == 0)
                        ++nulls;
                assert((nulls & 1) == 0);
#endif

                if (!isError())
                    state = kBeginning;
            }

            if (_propertiesRemaining.size == 0) {
                // Read/decompress the frame into _in:
                readFrame(codec, int(mode), frame, frameFlags);
            }

            slice checksumSlice{checksum, Codec::kChecksumSize};
            codec.readAndVerifyChecksum(checksumSlice);

            bodyBytesReceived = _in->bytesWritten();

            if (!(frameFlags & kMoreComing)) {
                // Completed!
                if (_propertiesRemaining.size > 0)
                    throw std::runtime_error("message ends before end of properties");
                _body = _in->finish();
                _in.reset();
                _complete = true;

                _connection->logVerbose("Finished receiving %s #%llu, flags=%02x",
                                 kMessageTypeNames[type()], _number, flags());
                state = kEnd;
            }
        }
        // ...mutex is now unlocked

        // Send progress. ("kReceivingReply" is somewhat misleading if this isn't a reply.)
        // Include a pointer to myself when my properties are available, _unless_ I'm an
        // incomplete error. (We need the error body first since it contains the message.)
        bool includeThis = (state == kEnd || (_properties && !isError()));
        sendProgress(state == kEnd ? MessageProgress::kComplete : MessageProgress::kReceivingReply,
                     _outgoingSize, bodyBytesReceived,
                     (includeThis ? this : nullptr));
        return state;
    }


    void MessageIn::acknowledge(size_t frameSize) {
        _unackedBytes += frameSize;
        if (_unackedBytes >= kIncomingAckThreshold) {
            // Send an ACK after enough data has been received of this message:
            MessageType msgType = isResponse() ? kAckResponseType : kAckRequestType;
            uint8_t buf[kMaxVarintLen64];
            alloc_slice payload(buf, PutUVarInt(buf, _rawBytesReceived));
            Retained<MessageOut> ack = new MessageOut(_connection,
                                                      (FrameFlags)(msgType | kUrgent | kNoReply),
                                                      payload,
                                                      nullptr,
                                                      _number);
            _connection->send(ack);
            _unackedBytes = 0;
        }
    }


    void MessageIn::readFrame(Codec &codec, int mode, slice &frame, bool finalFrame) {
        uint8_t buffer[4096];
        while (frame.size > 0) {
            slice output {buffer, sizeof(buffer)};
            codec.write(frame, output, Codec::Mode(mode));
            if (output.buf > buffer)
                _in->writeRaw(slice(buffer, output.buf));
        }
    }


    void MessageIn::setProgressCallback(MessageProgressCallback callback) {
        lock_guard<mutex> lock(_receiveMutex);
        _onProgress = callback;
    }


    bool MessageIn::isComplete() const {
        lock_guard<mutex> lock(const_cast<MessageIn*>(this)->_receiveMutex);
        return _complete;
    }


#pragma mark - MESSAGE BODY:


    alloc_slice MessageIn::body() const {
        lock_guard<mutex> lock(const_cast<MessageIn*>(this)->_receiveMutex);
        return _body;
    }


    fleeceapi::Value MessageIn::JSONBody() {
        lock_guard<mutex> lock(_receiveMutex);
        if (!_bodyAsFleece)
            _bodyAsFleece = FLData_ConvertJSON({_body.buf, _body.size}, nullptr);
        return fleeceapi::Value::fromData(_bodyAsFleece);
    }


    alloc_slice MessageIn::extractBody() {
        lock_guard<mutex> lock(_receiveMutex);
        alloc_slice body = _body;
        if (body) {
            _body = nullslice;
        } else if (_in) {
            body = _in->finish();
            _in->reset();
        }
        return body;
    }


#pragma mark - RESPONSES:


    void MessageIn::respond(MessageBuilder &mb) {
        if (noReply()) {
            _connection->warn("Ignoring attempt to respond to a noReply message");
            return;
        }
        if (mb.type == kRequestType)
            mb.type = kResponseType;
        Retained<MessageOut> message = new MessageOut(_connection, mb, _number);
        _connection->send(message);
    }


    void MessageIn::respondWithError(Error err) {
        if (!noReply()) {
            MessageBuilder mb(this);
            mb.makeError(err);
            respond(mb);
        }
    }


    void MessageIn::respond() {
        if (!noReply()) {
            MessageBuilder reply(this);
            respond(reply);
        }
    }


    void MessageIn::notHandled() {
        respondWithError({"BLIP"_sl, 404, "no handler for message"_sl});
    }


#pragma mark - PROPERTIES:


    slice MessageIn::property(slice property) const {
        uint8_t specbuf[1] = { MessageBuilder::tokenizeProperty(property) };
        if (specbuf[0])
            property = slice(&specbuf, 1);

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

    
    Error MessageIn::getError() const {
        if (!isError())
            return Error();
        return Error(property("Error-Domain"_sl),
                     (int) intProperty("Error-Code"_sl),
                     body());
    }

} }
