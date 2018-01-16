//
//  BLIPConnection.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "BLIPConnection.hh"
#include "MessageOut.hh"
#include "BLIPInternal.hh"
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include "PlatformCompat.hh"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <mutex>
#include <map>
#include <unordered_map>
#include <vector>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::websocket;

namespace litecore { namespace blip {

    static const size_t kDefaultFrameSize = 4096;       // Default size of frame
    static const size_t kBigFrameSize = 16384;          // Max size of frame

    static const auto kDefaultCompressionLevel = (Deflater::CompressionLevel)6;

    const char* const kMessageTypeNames[8] = {"REQ", "RES", "ERR", "?3?",
                                              "ACKREQ", "AKRES", "?6?", "?7?"};

    LogDomain BLIPLog("BLIP");
    static LogDomain BLIPMessagesLog("BLIPMessages", LogLevel::None);


    /** Queue of outgoing messages; each message gets to send one frame in turn. */
    class MessageQueue : public vector<Retained<MessageOut>> {
    public:
        MessageQueue()                          { }
        MessageQueue(size_t rsrv)               {reserve(rsrv);}

        bool contains(MessageOut *msg) const    {return find(begin(), end(), msg) != end();}

        MessageOut* findMessage(MessageNo msgNo, bool isResponse) const {
            auto i = find_if(begin(), end(), [&](const Retained<MessageOut> &msg) {
                return msg->number() == msgNo && msg->isResponse() == isResponse;
            });
            return (i != end()) ? *i : nullptr;
        }

        Retained<MessageOut> pop() {
            if (empty())
                return nullptr;
            Retained<MessageOut> msg(front());
            erase(begin());
            return msg;
        }

        bool remove(MessageOut *msg) {
            auto i = find(begin(), end(), msg);
            if (i == end())
                return false;
            erase(i);
            return true;
        }
    };


#pragma mark - BLIP I/O:


    /** The guts of a Connection. */
    class BLIPIO : public actor::Actor, public Logging, public websocket::Delegate {
    private:
        using MessageMap = unordered_map<MessageNo, Retained<MessageIn>>;
        using HandlerKey = pair<string, bool>;
        using RequestHandlers = map<HandlerKey, Connection::RequestHandler>;

        Retained<Connection>    _connection;
        Retained<WebSocket>     _webSocket;
        bool                    _closingWithError {false};
        MessageQueue            _outbox;
        MessageQueue            _icebox;
        bool                    _writeable {true};
        MessageMap              _pendingRequests, _pendingResponses;
        atomic<MessageNo>       _lastMessageNo {0};
        MessageNo               _numRequestsReceived {0};
        Deflater                _outputCodec;
        Inflater                _inputCodec;
        unique_ptr<uint8_t[]>   _frameBuf;
        RequestHandlers         _requestHandlers;
        size_t                  _maxOutboxDepth {0}, _totalOutboxDepth {0}, _countOutboxDepth {0};
        uint64_t                _totalBytesWritten {0}, _totalBytesRead {0};

    public:

        BLIPIO(Connection *connection, WebSocket *webSocket, Deflater::CompressionLevel compressionLevel)
        :Actor(string("BLIP[") + connection->name() + "]")
        ,Logging(BLIPLog)
        ,_connection(connection)
        ,_webSocket(webSocket)
        ,_outbox(10)
        ,_outputCodec(compressionLevel)
        {
            _pendingRequests.reserve(10);
            _pendingResponses.reserve(10);
        }

        void start() {
            _webSocket->connect(this);
            retain(this); // keep myself from being freed while I'm the webSocket's delegate
        }

        void queueMessage(MessageOut *msg) {
            enqueue(&BLIPIO::_queueMessage, Retained<MessageOut>(msg));
        }

        void setRequestHandler(std::string profile, bool atBeginning,
                               Connection::RequestHandler handler) {
            enqueue(&BLIPIO::_setRequestHandler, profile, atBeginning, handler);
        }

        void close() {
            enqueue(&BLIPIO::_close);
        }

        WebSocket* webSocket() const {
            return _webSocket;
        }

        virtual std::string loggingIdentifier() const override {
            return _connection ? _connection->name() : Logging::loggingIdentifier();
        }


    protected:

        ~BLIPIO() {
            log("~BLIPIO: Sent %llu bytes, rcvd %llu. Max outbox depth was %zu, avg %.2f",
                _totalBytesWritten, _totalBytesRead,
                _maxOutboxDepth, _totalOutboxDepth/(double)_countOutboxDepth);
        }

        virtual void onWebSocketGotHTTPResponse(int status,
                                                const fleeceapi::AllocedDict &headers) override
        {
            _connection->gotHTTPResponse(status, headers);
        }

        // websocket::Delegate interface:
        virtual void onWebSocketConnect() override {
            _connection->connected();
            onWebSocketWriteable();
        }

        virtual void onWebSocketClose(websocket::CloseStatus status) override {
            enqueue(&BLIPIO::_closed, status);
        }

        virtual void onWebSocketWriteable() override {
            enqueue(&BLIPIO::_onWebSocketWriteable);
        }

        virtual void onWebSocketMessage(slice message, bool binary) override {
            enqueue(&BLIPIO::_onWebSocketMessage, alloc_slice(message), binary);
        }

    private:

        /** Implementation of public close() method. Closes the WebSocket. */
        void _close() {
            if (_webSocket && !_closingWithError) {
                _webSocket->close();
            }
        }

        void _closeWithError(const char *msg) {
            if (_webSocket && !_closingWithError) {
                _webSocket->close(kCodeAbnormal, slice(msg));
                _closingWithError = true;
            }
        }

        void _closed(websocket::CloseStatus status) {
            _webSocket = nullptr;
            if (_connection) {
                Retained<BLIPIO> holdOn (this);
                _connection->closed(status);
                _connection = nullptr;
                cancelAll(_outbox);
                cancelAll(_icebox);
                cancelAll(_pendingRequests);
                cancelAll(_pendingResponses);
                _requestHandlers.clear();
                release(this); // webSocket is done calling delegate now (balances retain in ctor)
            }
        }


#pragma mark OUTGOING:


        /** Implementation of public queueMessage() method.
            Adds a new message to the outgoing queue and wakes up the queue. */
        void _queueMessage(Retained<MessageOut> msg) {
            if (!_webSocket || _closingWithError) {
                log("Can't send %s #%llu; socket is closed",
                    kMessageTypeNames[msg->type()], msg->number());
                msg->disconnected();
                return;
            }
            if (msg->_number == 0)
                msg->_number = ++_lastMessageNo;
            if (!msg->isAck() || BLIPLog.willLog(LogLevel::Verbose)) {
                logVerbose("Sending %s #%llu, flags=%02x",
                           kMessageTypeNames[msg->type()], msg->_number, msg->flags());
            }
            _maxOutboxDepth = max(_maxOutboxDepth, _outbox.size()+1);
            _totalOutboxDepth += _outbox.size()+1;
            ++_countOutboxDepth;
            requeue(msg, true);
        }


        /** Adds a message to the outgoing queue */
        void requeue(MessageOut *msg, bool andWrite =false) {
            assert(!_outbox.contains(msg));
            auto i = _outbox.end();
            if (msg->urgent() && _outbox.size() > 1) {
                // High-priority gets queued after the last existing high-priority message,
                // leaving one regular-priority message in between if possible:
                const bool isNew = (msg->_bytesSent == 0);
                do {
                    --i;
                    if ((*i)->urgent()) {
                        if ((i+1) != _outbox.end())
                            ++i;
                        break;
                    } else if (isNew && (*i)->_bytesSent == 0) {
                        break;
                    }
                } while (i != _outbox.begin());
                ++i;
            }
            _outbox.emplace(i, msg);  // inserts _at_ position i, before message *i

            if (andWrite)
                writeToWebSocket();
        }
        

        /** Adds an outgoing message to the icebox (until an ACK arrives.) */
        void freezeMessage(MessageOut *msg) {
            logVerbose("Freezing %s #%llu", kMessageTypeNames[msg->type()], msg->number());
            assert(!_outbox.contains(msg));
            assert(!_icebox.contains(msg));
            _icebox.push_back(msg);
        }


        /** Removes an outgoing message from the icebox and re-queues it (after ACK arrives.) */
        void thawMessage(MessageOut *msg) {
            logVerbose("Thawing %s #%llu", kMessageTypeNames[msg->type()], msg->number());
            LITECORE_UNUSED bool removed = _icebox.remove(msg);
            assert(removed);
            requeue(msg, true);
        }


        /** WebSocketDelegate method -- socket has room to write data. */
        void _onWebSocketWriteable() {
            logVerbose("WebSocket is hungry!");
            _writeable = true;
            writeToWebSocket();
        }


        /** Sends the next frame. */
        void writeToWebSocket() {
            if (!_writeable)
                return;

            //logVerbose("Writing to WebSocket...");
            size_t bytesWritten = 0;
            while (_writeable) {
                // Get the next message, if any, from the queue:
                Retained<MessageOut> msg(_outbox.pop());
                if (!msg)
                    break;

                FrameFlags frameFlags;
                {
                    // Set up a buffer for the frame contents:
                    size_t maxSize = kDefaultFrameSize;
                    if (msg->urgent() || _outbox.empty() || !_outbox.front()->urgent())
                        maxSize = kBigFrameSize;

                    if (!_frameBuf)
                        _frameBuf.reset(new uint8_t[kMaxVarintLen64 + 1 + 4 + kBigFrameSize]);
                    slice out(_frameBuf.get(), maxSize);
                    WriteUVarInt(&out, msg->_number);
                    auto flagsPos = (FrameFlags*)out.buf;
                    out.moveStart(1);

                    // Ask the MessageOut to write data to fill the buffer:
                    auto prevBytesSent = msg->_bytesSent;
                    msg->nextFrameToSend(_outputCodec, out, frameFlags);
                    *flagsPos = frameFlags;
                    slice frame(_frameBuf.get(), out.buf);
                    bytesWritten += frame.size;

                    logVerbose("    Sending frame: %s #%llu %c%c%c%c, bytes %u--%u",
                               kMessageTypeNames[frameFlags & kTypeMask], msg->number(),
                               (frameFlags & kMoreComing ? 'M' : '-'),
                               (frameFlags & kUrgent ? 'U' : '-'),
                               (frameFlags & kNoReply ? 'N' : '-'),
                               (frameFlags & kCompressed ? 'C' : '-'),
                               prevBytesSent, msg->_bytesSent - 1);
                    logVerbose("    %s", frame.hexString().c_str());//TEMP
                    // Write it to the WebSocket:
                    _writeable = _webSocket->send(frame);
                }
                
                // Return message to the queue if it has more frames left to send:
                if (frameFlags & kMoreComing) {
                    if (msg->needsAck())
                        freezeMessage(msg);
                    else
                        requeue(msg);
                } else {
                    if (!msg->isAck() || BLIPLog.willLog(LogLevel::Verbose)) {
                        logVerbose("Finished sending %s #%llu, flags=%02x",
                                   kMessageTypeNames[msg->type()], msg->_number, msg->flags());
                        // Add its response message to _pendingResponses:
                        MessageIn* response = msg->createResponse();
                        if (response)
                            _pendingResponses.emplace(response->number(), response);
                        
                    }
                }
            }
            _totalBytesWritten += bytesWritten;
            logVerbose("...Wrote %zu bytes to WebSocket (writeable=%d)",
                       bytesWritten, _writeable);
        }


#pragma mark INCOMING:

        
        /** WebSocketDelegate method -- Received a frame: */
        void _onWebSocketMessage(alloc_slice frame, bool binary) {
            try {
                if (_closingWithError)
                    return;
                if (!binary) {
                    warn("Ignoring non-binary WebSocket message");
                    return;
                }
                _totalBytesRead += frame.size;

                // Read the frame header:
                slice payload = frame;
                uint64_t msgNo, flagsInt;
                if (!ReadUVarInt(&payload, &msgNo) || !ReadUVarInt(&payload, &flagsInt))
                    throw runtime_error("Illegal BLIP frame header");
                auto flags = (FrameFlags)flagsInt;
                logVerbose("Received frame: %s #%llu %c%c%c%c, length %5ld",
                           kMessageTypeNames[flags & kTypeMask], msgNo,
                           (flags & kMoreComing ? 'M' : '-'),
                           (flags & kUrgent ? 'U' : '-'),
                           (flags & kNoReply ? 'N' : '-'),
                           (flags & kCompressed ? 'C' : '-'),
                           (long)payload.size);

                // Handle the frame according to its type, and look up the MessageIn:
                Retained<MessageIn> msg;
                auto type = (MessageType)(flags & kTypeMask);
                switch (type) {
                    case kRequestType:
                        msg = pendingRequest(msgNo, flags);
                        break;
                    case kResponseType:
                    case kErrorType: {
                        msg = pendingResponse(msgNo, flags);
                        break;
                    case kAckRequestType:
                    case kAckResponseType:
                        receivedAck(msgNo, (type == kAckResponseType), payload);
                        break;
                    default:
                        warn("  Unknown BLIP frame type received");
                        // For forward compatibility let's just ignore this instead of closing
                        break;
                    }
                }

                // Append the frame to the message:
                if (msg) {
                    auto state = msg->receivedFrame(_inputCodec, payload, flags);

                    if (state == MessageIn::kEnd) {
                        if (BLIPMessagesLog.willLog(LogLevel::Info)) {
                            stringstream dump;
                            bool withBody = BLIPMessagesLog.willLog(LogLevel::Verbose);
                            msg->dump(dump, withBody);
                            BLIPMessagesLog.log(LogLevel::Info, "RECEIVED: %s", dump.str().c_str());
                        }
                    }

                    if (type == kRequestType) {
                        if (state == MessageIn::kEnd || state == MessageIn::kBeginning) {
                            // Message complete!
                            handleRequestReceived(msg, state);
                        }
                    }
                }

            } catch (const std::exception &x) {
                logError("Caught exception handling incoming BLIP message: %s", x.what());
                _closeWithError(x.what());
            } catch (...) {
                logError("Caught unknown exception handling incoming BLIP message");
                _closeWithError("Unknown exception handling incoming BLIP message");
            }
        }


        /** Handle an incoming ACK message, by unfreezing the associated outgoing message. */
        void receivedAck(MessageNo msgNo, bool onResponse, slice body) {
            // Find the MessageOut in either _outbox or _icebox:
            bool frozen = false;
            Retained<MessageOut> msg = _outbox.findMessage(msgNo, onResponse);
            if (!msg) {
                msg = _icebox.findMessage(msgNo, onResponse);
                if (!msg) {
                    //logVerbose("Received ACK of non-current message (%s #%llu)",
                    //      (onResponse ? "RES" : "REQ"), msgNo);
                    return;
                }
                frozen = true;
            }

            // Acks have no checksum and don't go through the codec; just read the byte count:
            uint32_t byteCount;
            if (!ReadUVarInt32(&body, &byteCount)) {
                warn("Couldn't parse body of ACK");
                return;
            }
            
            msg->receivedAck(byteCount);
            if (frozen && !msg->needsAck())
                thawMessage(msg);
        }


        /** Returns the MessageIn object for the incoming request with the given MessageNo. */
        Retained<MessageIn> pendingRequest(MessageNo msgNo, FrameFlags flags) {
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
                throw runtime_error(format("BLIP protocol error: Bad incoming REQ #%llu (%s)",
                         msgNo, (msgNo <= _numRequestsReceived ? "already finished" : "too high")));
            }
            return msg;
        }


        /** Returns the MessageIn object for the incoming response with the given MessageNo. */
        Retained<MessageIn> pendingResponse(MessageNo msgNo, FrameFlags flags) {
            Retained<MessageIn> msg;
            auto i = _pendingResponses.find(msgNo);
            if (i != _pendingResponses.end()) {
                msg = i->second;
                if (!(flags & kMoreComing))
                    _pendingResponses.erase(i);
            } else {
                throw runtime_error(format("BLIP protocol error: Bad incoming RES #%llu (%s)",
                       msgNo, (msgNo <= _lastMessageNo ? "no request waiting" : "too high")));
            }
            return msg;
        }


        void cancelAll(MessageQueue &queue) {   // either _outbox or _icebox
            if (!queue.empty())
                log("Notifying %zd outgoing messages they're canceled", queue.size());
            for (auto &msg : queue)
                msg->disconnected();
            queue.clear();
        }

        void cancelAll(MessageMap &pending) {   // either _pendingResponses or _pendingRequests
            if (!pending.empty())
                log("Notifying %zd incoming messages they're canceled", pending.size());
            for (auto &item : pending)
                item.second->disconnected();
            pending.clear();
        }


        void _setRequestHandler(std::string profile, bool atBeginning,
                                Connection::RequestHandler handler)
        {
            HandlerKey key{profile, atBeginning};
            if (handler)
                _requestHandlers.emplace(key, handler);
            else
                _requestHandlers.erase(key);
        }


        void handleRequestBeginning(MessageIn *request) {
            
        }

        void handleRequestReceived(MessageIn *request, MessageIn::ReceiveState state) {
            try {
                if (state == MessageIn::kOther)
                    return;
                bool beginning = (state == MessageIn::kBeginning);
                auto profile = request->property("Profile"_sl);
                if (profile) {
                    auto i = _requestHandlers.find({profile.asString(), beginning});
                    if (i != _requestHandlers.end()) {
                        i->second(request);
                        return;
                    }
                }
                // No handler; just pass it to the delegate:
                if (beginning)
                    _connection->delegate().onRequestBeginning(request);
                else
                    _connection->delegate().onRequestReceived(request);
            } catch (...) {
                logError("Caught exception thrown from BLIP request handler");
                request->respondWithError({"BLIP"_sl, 501, "unexpected exception"_sl});
            }
        }

    }; // end of class BLIPIO


#pragma mark - CONNECTION:


    Connection::Connection(const Address &address,
                           Provider &provider,
                           const fleeceapi::AllocedDict &options,
                           ConnectionDelegate &delegate)
    :Logging(BLIPLog)
    ,_name(string("->") + (string)address)
    ,_isServer(false)
    ,_delegate(delegate)
    {
        log("Opening connection...");
        setWebSocket(provider.createWebSocket(address, options), options);
    }


    Connection::Connection(WebSocket *webSocket,
                           const fleeceapi::AllocedDict &options,
                           ConnectionDelegate &delegate)
    :Logging(BLIPLog)
    ,_name(string("<-") + (string)webSocket->address())
    ,_isServer(true)
    ,_delegate(delegate)
    {
        log("Accepted connection");
        setWebSocket(webSocket, options);
    }


    Connection::~Connection()
    {
        logDebug("~Connection");
    }


    void Connection::setWebSocket(WebSocket *webSocket, const fleeceapi::AllocedDict &options) {
        _compressionLevel = kDefaultCompressionLevel;
        auto levelP = options[kCompressionLevelOption];
        if (levelP.isInteger())
            _compressionLevel = (int8_t)levelP.asInt();

        webSocket->name = _name;
        
        // Now connect the websocket:
        _io = new BLIPIO(this, webSocket, (Deflater::CompressionLevel)_compressionLevel);
    }


    void Connection::start() {
        Assert(_state == kClosed);
        _state = kConnecting;
        _io->start();
    }


    /** Public API to send a new request. */
    void Connection::sendRequest(MessageBuilder &mb) {
        Retained<MessageOut> message = new MessageOut(this, mb, 0);
        assert(message->type() == kRequestType);
        send(message);
    }


    /** Internal API to send an outgoing message (a request, response, or ACK.) */
    void Connection::send(MessageOut *msg) {
        if (_compressionLevel == 0)
            msg->dontCompress();
        if (BLIPMessagesLog.willLog(LogLevel::Info)) {
            stringstream dump;
            bool withBody = BLIPMessagesLog.willLog(LogLevel::Verbose);
            msg->dump(dump, withBody);
            BLIPMessagesLog.log(LogLevel::Info, "SENDING: %s", dump.str().c_str());
        }

        _io->queueMessage(msg);
    }


    void Connection::setRequestHandler(string profile, bool atBeginning, RequestHandler handler) {
        _io->setRequestHandler(profile, atBeginning, handler);
    }


    void Connection::gotHTTPResponse(int status, const fleeceapi::AllocedDict &headers) {
        delegate().onHTTPResponse(status, headers);
    }


    void Connection::connected() {
        log("Connected!");
        _state = kConnected;
        delegate().onConnect();
    }


    void Connection::close() {
        log("Close connection");
        _state = kClosing;
        _io->close();
    }


    void Connection::closed(CloseStatus status) {
        log("Closed with %-s %d: %.*s",
              status.reasonName(), status.code,
              (int)status.message.size, status.message.buf);
        _state = status.isNormal() ? kClosed : kDisconnected;
        _closeStatus = status;
        delegate().onClose(status, _state);
    }


    websocket::WebSocket* Connection::webSocket() const {
        return _io->webSocket();
    }

} }
