//
// MessageOut.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "MessageOut.hh"
#include "BLIPConnection.hh"
#include "Codec.hh"
#include "Error.hh"
#include <algorithm>
#include <utility>

using namespace std;
using namespace fleece;

namespace litecore::blip {

    static const size_t kDataBufferSize = 16384;

    MessageOut::MessageOut(Connection* connection, FrameFlags flags, const alloc_slice& payload,
                           MessageDataSource&& dataSource, MessageNo number)
        : Message(flags, number), _connection(connection), _contents(payload, std::move(dataSource)) {}

    MessageOut::MessageOut(Connection *connection,
                           BuiltMessage &&built,
                           MessageNo number)
    :MessageOut(connection,
                built._flags,
                move(built._payload),
                move(built.dataSource),
                number)
    {
        _onProgress = move(built.onProgress);
    }


    void MessageOut::nextFrameToSend(Codec &codec, slice_ostream &dst, FrameFlags &outFlags) {
        outFlags = flags();
        if ( isAck() ) {
            // Acks have no checksum and don't go through the codec
            slice& data = _contents.dataToSend();
            dst.write(data);
            _bytesSent += (uint32_t)data.size;
            return;
        }

        // Write the frame:
        size_t frameSize = dst.capacity();
        {
            // `frame` is the same as `dst` but 4 bytes shorter, to leave space for the checksum
            slice_ostream frame(dst.next(), frameSize - Codec::kChecksumSize);
            auto          mode = hasFlag(kCompressed) ? Codec::Mode::SyncFlush : Codec::Mode::Raw;
            do {
                slice_istream& data = _contents.dataToSend();
                if ( data.size == 0 ) break;
                _uncompressedBytesSent += (uint32_t)data.size;
                codec.write(data, frame, mode);
                _uncompressedBytesSent -= (uint32_t)data.size;
            } while ( frame.capacity() >= 1024 );

            if ( codec.unflushedBytes() > 0 ) throw runtime_error("Compression buffer overflow");

            if ( mode == Codec::Mode::SyncFlush ) {
                size_t bytesWritten = (frameSize - Codec::kChecksumSize) - frame.capacity();
                if ( bytesWritten > 0 ) {
                    // SyncFlush always ends the output with the 4 bytes 00 00 FF FF.
                    // We can remove those, then add them when reading the data back in.
                    Assert(bytesWritten >= 4 && memcmp((const char*)frame.next() - 4, "\x00\x00\xFF\xFF", 4) == 0);
                    frame.retreat(4);
                }
            }

            // Write the checksum:
            dst.advanceTo(frame.next());  // Catch `dst` up to where `frame` is
            codec.writeChecksum(dst);
        }

        // Compute the (compressed) frame size, and update running totals:
        frameSize -= dst.capacity();
        _bytesSent += (uint32_t)frameSize;
        _unackedBytes += (uint32_t)frameSize;

        // Update flags & state:
        MessageProgress::State state;
        if ( _contents.hasMoreDataToSend() ) {
            outFlags = (FrameFlags)(outFlags | kMoreComing);
            state    = MessageProgress::kSending;
        } else if ( noReply() ) {
            state = MessageProgress::kComplete;
        } else {
            state = MessageProgress::kAwaitingReply;
        }
        sendProgress(state, _uncompressedBytesSent, 0, nullptr);
    }

    void MessageOut::receivedAck(uint32_t byteCount) {
        if ( byteCount <= _bytesSent ) _unackedBytes = min(_unackedBytes, (uint32_t)(_bytesSent - byteCount));
    }

    MessageIn* MessageOut::createResponse() {
        if ( type() != kRequestType || noReply() ) return nullptr;
        // Note: The MessageIn's flags will be updated when the 1st frame of the response arrives;
        // the type might become kErrorType, and kUrgent or kCompressed might be set.
        return new MessageIn(_connection, (FrameFlags)kResponseType, _number, _onProgress, _uncompressedBytesSent);
    }

    void MessageOut::disconnected() {
        if ( type() != kRequestType || noReply() ) return;
        Message::disconnected();
    }

    void MessageOut::dump(std::ostream& out, bool withBody) {
        auto [props, body] = getPropsAndBody();
        if ( !withBody ) body = nullslice;
        Message::dump(props, body, out);
    }

    const char* MessageOut::findProperty(const char* propertyName) {
        slice props, body;
        tie(props, body) = getPropsAndBody();
        return Message::findProperty(props, propertyName);
    }

    string MessageOut::description() {
        stringstream s;
        slice        props, body;
        tie(props, body) = getPropsAndBody();
        writeDescription(props, s);
        return s.str();
    }

#pragma mark - DATA:

    pair<slice, slice> MessageOut::getPropsAndBody() const {
        if ( type() < kAckRequestType ) return _contents.getPropsAndBody();
        else
            return {nullslice, _contents.body()};  // (ACKs do not have properties)
    }

    MessageOut::Contents::Contents(const alloc_slice& payload, MessageDataSource dataSource)
        : _payload(payload)
        , _unsentPayload(payload.buf, payload.size)
        , _dataSource(std::move(dataSource))
        , _unsentDataBuffer(nullslice) {
        DebugAssert(payload.size <= UINT32_MAX);
    }

    // Returns the next message-body data to send (as a slice _reference_)
    slice_istream& MessageOut::Contents::dataToSend() {
        if ( _unsentPayload.size > 0 ) {
            return _unsentPayload;
        } else {
            _payload.reset();
            if ( _unsentDataBuffer.size == 0 && _dataSource ) {
                readFromDataSource();
                if ( _unsentDataBuffer.size == 0 ) _dataBuffer.reset();
            }
            return _unsentDataBuffer;
        }
    }

    // Is there more data to send?
    bool MessageOut::Contents::hasMoreDataToSend() const {
        return _unsentPayload.size > 0 || _unsentDataBuffer.size > 0 || _dataSource != nullptr;
    }

    // Refills _dataBuffer and _dataBufferAvail from _dataSource.
    void MessageOut::Contents::readFromDataSource() {
        if ( !_dataBuffer ) _dataBuffer.reset(kDataBufferSize);
        auto bytesWritten = (*_dataSource)((void*)_dataBuffer.buf, _dataBuffer.size);
        _unsentDataBuffer = _dataBuffer.upTo(bytesWritten);
        if ( bytesWritten < _dataBuffer.size ) {
            // End of data source
            _dataSource = nullptr;
            if ( bytesWritten < 0 ) {
                WarnError("Error from BLIP message dataSource");
                //FIX: How to report/handle the error?
            }
        }
    }

    pair<slice, slice> MessageOut::Contents::getPropsAndBody() const {
        slice_istream in = _payload;
        if ( in.size > 0 ) {
            // This assumes the message starts with properties, which start with a UVarInt32.
            optional<uint32_t> propertiesSize = in.readUVarInt32();
            if ( !propertiesSize || *propertiesSize > in.size )
                error::_throw(error::CorruptData, "Invalid properties size in BLIP frame");
            in.setSize(*propertiesSize);
        } else if ( !in.buf ) {
            return {nullslice, nullslice};
        }
        return {in, slice(in.end(), _payload.end())};
    }

}  // namespace litecore::blip
