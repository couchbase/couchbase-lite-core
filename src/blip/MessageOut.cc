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
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {

    MessageOut::MessageOut(Connection *connection,
                           FrameFlags flags,
                           alloc_slice payload,
                           MessageDataSource dataSource,
                           MessageNo number)
    :Message(flags, number)
    ,_connection(connection)
    ,_payload(payload)
    ,_dataSource(dataSource)
    {
        assert(payload.size <= UINT32_MAX);
    }


    slice MessageOut::nextFrameToSend(size_t maxSize, FrameFlags &outFlags) {
        slice frame;
        bool moreComing;
        if (_bytesSent < _payload.size) {
            size_t size = min(maxSize, _payload.size - _bytesSent);
            frame = _payload(_bytesSent, size);
            moreComing = _bytesSent + size < _payload.size || _dataSource != nullptr;
        } else {
            if (_dataBuffer.size < maxSize)
                _dataBuffer.resize(maxSize);
            int size = _dataSource((void*)_dataBuffer.buf, maxSize);
            if (size < 0) {
                WarnError("Error from BLIP message dataSource");
                size = 0;
                //FIX: How to report/handle the error?
            }
            frame = slice(_dataBuffer.buf, size);
            moreComing = (size == maxSize);
        }
        _bytesSent += frame.size;
        _unackedBytes += frame.size;
        outFlags = flags();
        MessageProgress::State state;
        if (moreComing) {
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


    void MessageOut::disconnected() {
        if (type() != kRequestType || noReply())
            return;
        Message::disconnected();
    }

} }
