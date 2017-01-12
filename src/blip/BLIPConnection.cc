//
//  BLIP.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "BLIPConnection.hh"
#include "Message.hh"
#include "BLIPInternal.hh"
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Logging.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <mutex>
#include <vector>

using namespace std;
using namespace fleece;
using namespace litecore;

namespace litecore { namespace blip {

    static size_t kDefaultFrameSize = 4096;
    static size_t kBigFrameSize = 16384;

    static LogDomain BLIPLog("BLIP");


#pragma mark - BLIP I/O:


    /** The guts of a Connection. */
    class BLIPIO : public Actor, public WebSocketDelegate {
    public:
        BLIPIO(Connection*, Scheduler*);

        void queueMessage(MessageOut *msg) {
            msg->_number = ++_lastMessageNo;
            enqueue(&BLIPIO::_queueMessage, Retained<MessageOut>(msg));
        }

        void close() {
            enqueue(&BLIPIO::_close);
        }

    protected:
        // WebSocketDelegate interface:
        virtual void onConnect() override {
            _connection->delegate().onConnect();
            onWriteable();
        }
        virtual void onError(int errcode, fleece::slice reason) override {
            _connection->delegate().onError(errcode, reason);
        }
        virtual void onClose(int status, fleece::slice reason) override {
            _connection->delegate().onClose(status, reason);
        }

        virtual void onWriteable() override {
            enqueue(&BLIPIO::_onWriteable);
        }
        virtual void onMessage(fleece::slice message, bool binary) override {
            enqueue(&BLIPIO::_onMessage, alloc_slice(message), binary);
        }

    private:
        void _onWriteable();
        void _onMessage(alloc_slice message, bool binary);
        void _queueMessage(Retained<MessageOut> msg);
        void _close();

        Retained<MessageOut> popNextMessage();
        void requeue(MessageOut*);
        Retained<MessageIn> pendingRequest(MessageNo msgNo, FrameFlags);
        Retained<MessageIn> pendingResponse(MessageNo msgNo, FrameFlags);

        Retained<Connection> _connection;
        vector<Retained<MessageOut>> _queue;
        bool _hungry {false};
        unordered_map<MessageNo, Retained<MessageIn>> _pendingRequests, _pendingResponses;
        atomic<MessageNo> _lastMessageNo {0};
        MessageNo _numRequestsReceived {0};
        uint8_t* _frameBuf {nullptr};
    };


    BLIPIO::BLIPIO(Connection *connection, Scheduler *scheduler)
    :Actor(scheduler)
    ,_connection(connection)
    { }


    void BLIPIO::_queueMessage(Retained<MessageOut> msg) {
        {
            LogTo(BLIPLog, "Enqueuing message #%llu, flags=%02x", msg->_number, msg->flags());
            requeue(msg);
        }
        if (_hungry)
            onWriteable();
    }


    void BLIPIO::requeue(MessageOut *msg) {
        auto i = _queue.end();
        if (msg->urgent() && !_queue.empty()) {
            // High-priority gets queued after the last existing high-priority message,
            // leaving one regular-priority message in between if possible:
            do {
                --i;
                if ((*i)->urgent()) {
                    if ((i+1) != _queue.end())
                        ++i;
                    break;
                } else if (msg->_bytesSent == 0 && (*i)->_bytesSent == 0) {
                    // But make sure to keep the 1st frames of messages in chronological order:
                    break;
                }
            } while (i != _queue.begin());
            ++i;
        }
        _queue.emplace(i, msg);  // inserts _at_ position i
    }


    Retained<MessageOut> BLIPIO::popNextMessage() {
        if (_queue.empty())
            return nullptr;
        Retained<MessageOut> msg(_queue.front());
        _queue.erase(_queue.begin());
        return msg;
    }


    // Send the next frame:
    void BLIPIO::_onWriteable() {
        LogTo(BLIPLog, "Socket is writeable");
        // Get the next message from the queue:
        Retained<MessageOut> msg(popNextMessage());

        // If there's nothing to send, just remember that I'm ready:
        _hungry = (msg == nullptr);
        if (_hungry)
            return;

        FrameFlags frameFlags;
        {
            // On first frame of a request, add its response message to _pendingResponses:
            if (msg->_bytesSent == 0) {
                MessageIn *response = msg->pendingResponse();
                if (response)
                    _pendingResponses.emplace(msg->_number, response);
            }

            // Read a frame from it:
            size_t maxSize = kDefaultFrameSize;
            if (msg->urgent() || _queue.empty() || !_queue.front()->urgent())
                maxSize = kBigFrameSize;

            slice body = msg->nextFrameToSend(maxSize - 10, frameFlags);

            LogTo(BLIPLog, "Sending frame: Msg #%llu, flags %02x, bytes %llu--%llu",
                  msg->_number, frameFlags,
                  (uint64_t)(msg->_bytesSent - body.size), (uint64_t)(msg->_bytesSent - 1));

            // Copy header and frame to a buffer, and send over the WebSocket:
            if (!_frameBuf)
                _frameBuf = new uint8_t[2*kMaxVarintLen64 + kBigFrameSize];
            uint8_t *end = _frameBuf;
            end += PutUVarInt(end, msg->_number);
            end += PutUVarInt(end, frameFlags);
            memcpy(end, body.buf, body.size);
            end += body.size;
            connection()->send({_frameBuf, end});
        }

        // Return message to the queue if it has more frames left to send:
        if (frameFlags & kMoreComing)
            requeue(msg);
    }


    // Received a frame:
    void BLIPIO::_onMessage(alloc_slice frame, bool binary) {
        if (!binary) {
            LogTo(BLIPLog, "Ignoring non-binary message");
            return;
        }
        uint64_t msgNo, flagsInt;
        if (!ReadUVarInt(&frame, &msgNo) || !ReadUVarInt(&frame, &flagsInt)) {
            LogToAt(BLIPLog, Warning, "Illegal frame header");
            return;
        }
        auto flags = (FrameFlags)flagsInt;
        LogTo(BLIPLog, "Received frame: Msg #%llu, flags %02x, length %ld",
              msgNo, flags, (long)frame.size);

        Retained<MessageIn> msg;
        {
            switch (flags & kTypeMask) {
                case kRequestType:
                    msg = pendingRequest(msgNo, flags);
                    break;
                case kResponseType:
                case kErrorType: {
                    msg = pendingResponse(msgNo, flags);
                    break;
                default:
                    LogTo(BLIPLog, "Unknown frame type received");
                    break;
                }
            }
        }
        if (msg)
            msg->receivedFrame(frame, flags);
    }


    /** Returns the MessageIn object for the incoming request with the given MessageNo. */
    Retained<MessageIn> BLIPIO::pendingRequest(MessageNo msgNo, FrameFlags flags) {
        Retained<MessageIn> msg;
        auto i = _pendingRequests.find(msgNo);
        if (i != _pendingRequests.end()) {
            // Existing request: return it, and remove from _pendingRequests if the last frame:
            msg = i->second;
            if (!(flags & kMoreComing))
                _pendingRequests.erase(i);
        } else if (msgNo == _numRequestsReceived + 1) {
            // New request: create and add to _pendingRequests unless it's a singleton frame:
            ++_numRequestsReceived;
            msg = new MessageIn(_connection, flags, msgNo);
            if (flags & kMoreComing)
                _pendingRequests.emplace(msgNo, msg);
        } else {
            LogToAt(BLIPLog, Warning, "Bad incoming request number %llu", msgNo);
        }
        return msg;
    }

    
    /** Returns the MessageIn object for the incoming response with the given MessageNo. */
    Retained<MessageIn> BLIPIO::pendingResponse(MessageNo msgNo, FrameFlags flags) {
        Retained<MessageIn> msg;
        auto i = _pendingResponses.find(msgNo);
        if (i != _pendingResponses.end()) {
            msg = i->second;
            if (!(flags & kMoreComing))
                _pendingResponses.erase(i);
        } else {
            LogToAt(BLIPLog, Warning, "Unexpected response to my message %llu", msgNo);
        }
        return msg;
    }


    void BLIPIO::_close() {
        connection()->close();
    }


#pragma mark - CONNECTION:


    static Scheduler* scheduler() {
        static Scheduler *sSched;
        if (!sSched) {
            sSched = new Scheduler;
            sSched->start();
        }
        return sSched;
    }


    Connection::Connection(const std::string &hostname, uint16_t port,
                           WebSocketProvider &provider,
                           ConnectionDelegate &delegate)
    :_delegate(delegate)
    ,_io(new BLIPIO(this, scheduler()))
    {
        delegate._connection = this;
        provider.addProtocol("BLIP");
        provider.connect(hostname, port, *_io);
    }


    Connection::~Connection()
    { }


    /** Public API to send a new request. */
    MessageIn* Connection::sendRequest(MessageBuilder &mb) {
        Retained<MessageOut> message = new MessageOut(this, mb);
        assert(message->type() == kRequestType);
        send(message);
        return message->pendingResponse();
    }


    void Connection::send(MessageOut *msg) {
        _io->queueMessage(msg);
    }


    void Connection::close() {
        _io->close();
    }

} }
