//
// Message.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Message.hh"
#include "MessageOut.hh"
#include "BLIPConnection.hh"
#include "Codec.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include "Instrumentation.hh"
#include <algorithm>
#include <cassert>
#include <memory>
#include <sstream>

#include <iostream>
#include <utility>

using namespace std;
using namespace fleece;

namespace litecore::blip {


    // How many bytes to receive before sending an ACK
    static const size_t kIncomingAckThreshold = 50000;

    void Message::sendProgress(MessageProgress::State state, MessageSize bytesSent, MessageSize bytesReceived,
                               MessageIn* reply) {
        if ( _onProgress ) _onProgress({state, bytesSent, bytesReceived, reply});
    }

    void Message::disconnected() { sendProgress(MessageProgress::kDisconnected, 0, 0, nullptr); }

    // Writes a slice to a stream. If it contains non-ASCII characters, it will be written as hex
    // inside "<<...>>". If empty, it's written as "<<>>".
    static ostream& dumpSlice(ostream& o, fleece::slice s) {
        if ( s.size == 0 ) return o << "<<>>";
        auto buf = (const uint8_t*)s.buf;
        for ( size_t i = 0; i < s.size; i++ ) {
            if ( buf[i] < 32 || buf[i] > 126 ) return o << "<<" << s.hexString() << ">>";
        }
        return o << s;
    }

    void Message::dumpHeader(std::ostream& out) {
        out << kMessageTypeNames[type()];
        out << " #" << _number << ' ';
        if ( _flags & kUrgent ) out << 'U';
        if ( _flags & kNoReply ) out << 'N';
        if ( _flags & kCompressed ) out << 'Z';
    }

    void Message::writeDescription(slice payload, std::ostream& out) {
        if ( type() == kRequestType ) {
            const char* profile = findProperty(payload, "Profile");
            if ( profile ) out << "'" << profile << "' ";
        }
        dumpHeader(out);
    }

    void Message::dump(slice payload, slice body, std::ostream& out) {
        dumpHeader(out);
        if ( type() != kAckRequestType && type() != kAckResponseType ) {
            out << " {";
            auto key = (const char*)payload.buf;
            auto end = (const char*)payload.end();
            while ( key < end ) {
                auto endOfKey = key + strlen(key);
                auto val      = endOfKey + 1;
                if ( val >= end ) break;  // illegal: missing value
                auto endOfVal = val + strlen(val);

                out << "\n\t";
                dumpSlice(out, {key, endOfKey});
                out << ": ";
                dumpSlice(out, {val, endOfVal});
                key = endOfVal + 1;
            }
            if ( body.size > 0 ) {
                out << "\n\tBODY: ";
                dumpSlice(out, body);
            }
            out << " }";
        }
    }

    const char* Message::findProperty(slice payload, const char* propertyName) {
        auto key = (const char*)payload.buf;
        auto end = (const char*)payload.end();
        while ( key < end ) {
            auto endOfKey = key + strlen(key);
            auto val      = endOfKey + 1;
            if ( val >= end ) break;  // illegal: missing value
            if ( 0 == strcmp(key, propertyName) ) return val;
            key = val + strlen(val) + 1;
        }
        return nullptr;
    }

#pragma mark - MESSAGEIN:


    MessageIn::~MessageIn() = default;

    MessageIn::MessageIn(Connection* connection, FrameFlags flags, MessageNo n, MessageProgressCallback onProgress,
                         MessageSize outgoingSize)
        : Message(flags, n), _connection(connection), _propertiesRemaining(nullptr, 0), _outgoingSize(outgoingSize) {
        _onProgress = std::move(onProgress);
    }

    MessageIn::ReceiveState MessageIn::receivedFrame(Codec& codec, slice entireFrame, FrameFlags frameFlags) {
        ReceiveState state = kOther;
        MessageSize  bodyBytesReceived;
        {
            // First, lock the mutex:
            lock_guard<mutex> lock(_receiveMutex);

            // Update byte count and send acknowledgement packet when appropriate:
            slice_istream frame(entireFrame);
            _rawBytesReceived += frame.size;
            acknowledge((uint32_t)frame.size);

            auto mode = (frameFlags & kCompressed) ? Codec::Mode::SyncFlush : Codec::Mode::Raw;

            // Copy and remove the checksum from the end of the frame:
            uint8_t checksum[Codec::kChecksumSize];
            auto    trailer = (void*)&frame[frame.size - Codec::kChecksumSize];
            memcpy(checksum, trailer, Codec::kChecksumSize);
            if ( mode == Codec::Mode::SyncFlush ) {
                // Replace checksum with the untransmitted deflate empty-block trailer,
                // which is conveniently the same size:
                static_assert(Codec::kChecksumSize == 4, "Checksum not same size as deflate trailer");
                memcpy(trailer, "\x00\x00\xFF\xFF", 4);
            } else {
                // In uncompressed message, just trim off the checksum:
                frame.setSize(frame.size - Codec::kChecksumSize);
            }

            bool justFinishedProperties = false;
            if ( !_in ) {
                // First frame!
                // Update my flags and allocate the Writer:
                DebugAssert(_number > 0);
                _flags = (FrameFlags)(frameFlags & ~kMoreComing);
                _in    = std::make_unique<fleece::JSONEncoder>();

                // Read just a few bytes to get the length of the properties (a varint at the
                // start of the frame):
                char          buf[kMaxVarintLen32];
                slice_ostream out(buf, sizeof(buf));
                codec.write(frame, out, mode);
                slice_istream dst = out.output();

                // Decode the properties length:
                if ( optional<uint32_t> n = dst.readUVarInt32(); !n ) throw std::runtime_error("frame too small");
                else
                    _propertiesSize = *n;
                if ( _propertiesSize > kMaxPropertiesSize ) throw std::runtime_error("properties excessively large");
                // Allocate properties and put any remaining decoded data there:
                _properties          = alloc_slice(_propertiesSize);
                _propertiesRemaining = slice_ostream(_properties);
                _propertiesRemaining.write(dst.readAtMost(_propertiesSize));
                if ( _propertiesRemaining.capacity() == 0 ) justFinishedProperties = true;
                // And anything left over after that becomes the start of the body:
                if ( dst.size > 0 ) _in->writeRaw(dst);
            }

            if ( _propertiesRemaining.capacity() > 0 ) {
                // Read into properties buffer:
                codec.write(frame, _propertiesRemaining, mode);
                if ( _propertiesRemaining.capacity() == 0 ) justFinishedProperties = true;
            }
            if ( justFinishedProperties ) {
                // Finished reading properties:
                if ( _propertiesSize > 0 && _properties[_propertiesSize - 1] != 0 )
                    throw std::runtime_error("message properties not null-terminated");
#if DEBUG
                int nulls = 0;
                for ( size_t i = 0; i < _propertiesSize; ++i )
                    if ( _properties[i] == 0 ) ++nulls;
                DebugAssert((nulls & 1) == 0);
#endif
                if ( _connection->willLog(LogLevel::Verbose) )
                    _connection->_logVerbose("Receiving %s", description().c_str());

                if ( !isError() ) state = kBeginning;
            }

            if ( _propertiesRemaining.capacity() == 0 ) {
                // Read/decompress the frame into _in:
                readFrame(codec, int(mode), frame, frameFlags);
            }

            slice_istream checksumSlice{checksum, Codec::kChecksumSize};
            codec.readAndVerifyChecksum(checksumSlice);

            bodyBytesReceived = expert(*_in).bytesWritten();

            if ( !(frameFlags & kMoreComing) ) {
                // Completed!
                if ( _propertiesRemaining.capacity() > 0 )
                    throw std::runtime_error("message ends before end of properties");
                _body = _in->finish();
                _in.reset();
                _complete = true;

                if ( _connection->willLog(LogLevel::Verbose) )
                    _connection->_logVerbose("Finished receiving %s", description().c_str());
                state = kEnd;
            }
        }
        // ...mutex is now unlocked

        // Send progress. ("kReceivingReply" is somewhat misleading if this isn't a reply.)
        // Include a pointer to myself when my properties are available, _unless_ I'm an
        // incomplete error. (We need the error body first since it contains the message.)
        bool includeThis = (state == kEnd || (_properties && !isError()));
        sendProgress(state == kEnd ? MessageProgress::kComplete : MessageProgress::kReceivingReply, _outgoingSize,
                     bodyBytesReceived, (includeThis ? this : nullptr));
        if ( state == kEnd ) Signpost::mark(Signpost::blipReceived, 0, static_cast<uintptr_t>(number()));
        return state;
    }

    void MessageIn::acknowledge(uint32_t frameSize) {
        _unackedBytes += frameSize;
        if ( _unackedBytes >= kIncomingAckThreshold ) {
            // Send an ACK after enough data has been received of this message:
            MessageType          msgType = isResponse() ? kAckResponseType : kAckRequestType;
            uint8_t              buf[kMaxVarintLen64];
            alloc_slice          payload(buf, PutUVarInt(buf, _rawBytesReceived));
            Retained<MessageOut> ack = new MessageOut(
                    _connection, (FrameFlags)(FrameFlags(msgType) | kUrgent | kNoReply), payload, nullptr, _number);
            _connection->send(ack);
            _unackedBytes = 0;
        }
    }

    void MessageIn::readFrame(Codec& codec, int mode, slice_istream& frame, bool /*finalFrame*/) {
        uint8_t buffer[4096];
        while ( frame.size > 0 ) {
            slice_ostream output(buffer, sizeof(buffer));
            codec.write(frame, output, Codec::Mode(mode));
            if ( output.bytesWritten() > 0 ) _in->writeRaw(output.output());
        }
    }

    void MessageIn::setProgressCallback(MessageProgressCallback callback) {
        lock_guard<mutex> lock(_receiveMutex);
        _onProgress = std::move(callback);
    }

    bool MessageIn::isComplete() const {
        lock_guard<mutex> lock(_receiveMutex);
        return _complete;
    }

#pragma mark - MESSAGE BODY:

    alloc_slice MessageIn::body() const {
        lock_guard<mutex> lock(_receiveMutex);
        return _body;
    }

    fleece::Value MessageIn::JSONBody() {
        lock_guard<mutex> lock(_receiveMutex);
        if ( !_bodyAsFleece ) {
            if ( _body.size == 0 ) {
                LogVerbose(kC4Cpp_DefaultLog, "MessageIn::JSONBody: body size is 0, returning null value...");
                return nullptr;
            }

            _bodyAsFleece = FLData_ConvertJSON({_body.buf, _body.size}, nullptr);
            if ( !_bodyAsFleece && _body != "null"_sl )
                Warn("MessageIn::JSONBody: Body does not contain valid JSON: %.*s", SPLAT(_body));
        }
        return fleece::ValueFromData(_bodyAsFleece);
    }

    alloc_slice MessageIn::extractBody() {
        lock_guard<mutex> lock(_receiveMutex);
        alloc_slice       body = _body;
        if ( body ) {
            _body = nullslice;
        } else if ( _in ) {
            body = _in->finish();
            _in->reset();
        }
        return body;
    }

#pragma mark - RESPONSES:

    void MessageIn::respond(MessageBuilder& mb) {
        if ( noReply() ) {
            _connection->warn("Ignoring attempt to respond to a noReply message");
            return;
        }
        Assert(!_responded);
        _responded = true;
        if ( mb.type == kRequestType ) mb.type = kResponseType;
        Retained<MessageOut> message = new MessageOut(_connection, mb, _number);
        _connection->send(message);
    }

    void MessageIn::respondWithError(Error err) {
        if ( !noReply() ) {
            MessageBuilder mb(this);
            mb.makeError(err);
            respond(mb);
        }
    }

    void MessageIn::respond() {
        if ( !noReply() ) {
            MessageBuilder reply(this);
            respond(reply);
        }
    }

    void MessageIn::notHandled() { respondWithError({"BLIP"_sl, 404, "no handler for message"_sl}); }

#pragma mark - PROPERTIES:

    slice MessageIn::property(slice property) const {
        // Note: using strlen here is safe. It can't fall off the end of _properties, because the
        // receivedFrame() method has already verified that _properties ends with a zero byte.
        // OPT: This lookup isn't very efficient. If it turns out to be a hot-spot, we could cache
        // the starting point of every property string.
        auto key = (const char*)_properties.buf;
        auto end = (const char*)_properties.end();
        while ( key < end ) {
            auto endOfKey = key + strlen(key);
            auto val      = endOfKey + 1;
            if ( val >= end ) break;  // illegal: missing value
            auto endOfVal = val + strlen(val);
            if ( property == slice(key, endOfKey) ) return {val, endOfVal};
            key = endOfVal + 1;
        }
        return nullslice;
    }

    long MessageIn::intProperty(slice name, long defaultValue) const {
        string value = property(name).asString();
        if ( value.empty() ) return defaultValue;
        char* end;
        long  result = strtol(value.c_str(), &end, 10);
        if ( *end != '\0' ) return defaultValue;
        return result;
    }

    bool MessageIn::boolProperty(slice name, bool defaultValue) const {
        slice value = property(name);
        if ( value.caseEquivalent("true"_sl) || value.caseEquivalent("YES"_sl) ) return true;
        else if ( value.caseEquivalent("false"_sl) || value.caseEquivalent("NO"_sl) )
            return false;
        else
            return intProperty(name, defaultValue) != 0;
    }

    Error MessageIn::getError() const {
        if ( !isError() ) return {};
        return {property("Error-Domain"_sl), (int)intProperty("Error-Code"_sl), body()};
    }

    string MessageIn::description() {
        stringstream s;
        writeDescription(_properties, s);
        return s.str();
    }


}  // namespace litecore::blip
