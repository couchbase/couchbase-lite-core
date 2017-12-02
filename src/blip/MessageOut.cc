//
//  MessageOut.cc
//  blip_cpp
//
//  Created by Jens Alfke on 4/5/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "MessageOut.hh"
#include "BLIPConnection.hh"
#include "BLIPInternal.hh"
#include "Codec.hh"
#include "varint.hh"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {

    static const size_t kDataBufferSize = 4096;

    MessageOut::MessageOut(Connection *connection,
                           FrameFlags flags,
                           alloc_slice payload,
                           MessageDataSource dataSource,
                           MessageNo number)
    :Message(flags, number)
    ,_connection(connection)
    ,_payload(payload)
    ,_unsentPayload(payload.buf, payload.size)
    ,_dataSource(dataSource)
    {
        assert(payload.size <= UINT32_MAX);
    }


    void MessageOut::nextFrameToSend(Codec &codec, slice &dst, FrameFlags &outFlags) {
        outFlags = flags();
        if (isAck()) {
            // Acks have no checksum and don't go through the codec
            dst.writeFrom(_unsentPayload);
            _bytesSent += _unsentPayload.size;
            return;
        }

        slice frame;
        size_t frameSize = dst.size;

        auto checksumPos = dst.read(4);     // Leave 4 bytes for checksum at start

        bool allWritten, moreComing;
        if (_unsentPayload.size > 0) {
            // Send data from my payload:
            allWritten = codec.write(_unsentPayload, dst);
            moreComing = _unsentPayload.size > 0 || _dataSource != nullptr;
        } else {
            // Send data from data-source:
            allWritten = moreComing = true;
            while (_dataSourceMoreComing && allWritten && dst.size >= 1024) {
                if (_dataBufferAvail.size == 0) {
                    readFromDataSource();
                    moreComing = _dataSourceMoreComing;
                }
                allWritten = codec.write(_dataBufferAvail, dst);
            }
        }

        if (!allWritten)
            throw runtime_error("Compression buffer overflow");

        codec.writeChecksum(checksumPos);   // Fill in the checksum

        frameSize -= dst.size;              // Compute the compressed frame size
        _bytesSent += frameSize;
        _unackedBytes += frameSize;
        
        MessageProgress::State state;
        if (moreComing) {
            outFlags = (FrameFlags)(outFlags | kMoreComing);
            state = MessageProgress::kSending;
        } else if (noReply()) {
            state = MessageProgress::kComplete;
        } else {
            state = MessageProgress::kAwaitingReply;
        }
        sendProgress(state, _payload.size - _unsentPayload.size, 0, nullptr);
    }


    void MessageOut::readFromDataSource() {
        if (!_dataBuffer)
            _dataBuffer.reset(kDataBufferSize);
        auto bytesWritten = _dataSource((void*)_dataBuffer.buf, _dataBuffer.size);
        _dataBufferAvail = _dataBuffer.upTo(bytesWritten);
        if (bytesWritten < _dataBuffer.size) {
            _dataSourceMoreComing = false;
            if (bytesWritten < 0) {
                WarnError("Error from BLIP message dataSource");
                //FIX: How to report/handle the error?
            }
        }
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


    void MessageOut::disconnected() {
        if (type() != kRequestType || noReply())
            return;
        Message::disconnected();
    }


    void MessageOut::dump(std::ostream& out, bool withBody) {
        slice props = _payload;
        uint32_t propertiesSize;
        ReadUVarInt32(&props, &propertiesSize);
        props.setSize(propertiesSize);
        slice body;
        if (withBody)
            body = slice(props.end(), _payload.end());
        Message::dump(props, body, out);
    }

} }
