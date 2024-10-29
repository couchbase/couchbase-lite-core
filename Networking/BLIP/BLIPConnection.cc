//
// BLIPConnection.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "BLIPConnection.hh"
#include "MessageOut.hh"
#include "BLIPInternal.hh"
#include "WebSocketImpl.hh"
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Batcher.hh"
#include "Codec.hh"
#include "Error.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "Stopwatch.hh"
#include "varint.hh"
#include "fleece/PlatformCompat.hh"
#include <algorithm>
#include <cassert>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::websocket;

namespace litecore::blip {

    static const size_t kDefaultFrameSize = 4096;   // Default size of frame
    static const size_t kBigFrameSize     = 16384;  // Max size of frame

    static const auto kDefaultCompressionLevel = (Deflater::CompressionLevel)6;

    const char* const kMessageTypeNames[8] = {"REQ", "RES", "ERR", "?3?", "ACKREQ", "AKRES", "?6?", "?7?"};

    LogDomain        BLIPLog("BLIP", LogLevel::Warning);
    static LogDomain BLIPMessagesLog("BLIPMessages", LogLevel::None);

    /** Queue of outgoing messages; each message gets to send one frame in turn. */
    class MessageQueue : public vector<Retained<MessageOut>> {
      public:
        MessageQueue() = default;

        explicit MessageQueue(size_t rsrv) { reserve(rsrv); }

        bool contains(MessageOut* msg) const { return find(begin(), end(), msg) != end(); }

        [[nodiscard]] MessageOut* findMessage(MessageNo msgNo, bool isResponse) const {
            auto i = find_if(begin(), end(), [&](const Retained<MessageOut>& msg) {
                return msg->number() == msgNo && msg->isResponse() == isResponse;
            });
            if ( i == end() ) return nullptr;
            return *i;
        }

        Retained<MessageOut> pop() {
            if ( empty() ) return nullptr;
            Retained<MessageOut> msg(front());
            erase(begin());
            return msg;
        }

        bool remove(MessageOut* msg) {
            auto i = find(begin(), end(), msg);
            if ( i == end() ) return false;
            erase(i);
            return true;
        }
    };

#pragma mark - BLIP I/O:

    /** The guts of a Connection. */
    class BLIPIO final
        : public actor::Actor
        , public websocket::Delegate {
      private:
        using MessageMap      = unordered_map<MessageNo, Retained<MessageIn>>;
        using HandlerKey      = pair<string, bool>;
        using RequestHandlers = map<HandlerKey, Connection::RequestHandler>;

        Retained<Connection>                            _connection;
        Retained<WebSocket>                             _webSocket;
        unique_ptr<error>                               _closingWithError;
        actor::ActorBatcher<BLIPIO, websocket::Message> _incomingFrames;
        MessageQueue                                    _outbox;
        MessageQueue                                    _icebox;
        bool                                            _writeable{true};
        MessageMap                                      _pendingRequests, _pendingResponses;
        atomic<MessageNo>                               _lastMessageNo{0};
        MessageNo                                       _numRequestsReceived{0};
        Deflater                                        _outputCodec;
        Inflater                                        _inputCodec;
        unique_ptr<uint8_t[]>                           _frameBuf;
        RequestHandlers                                 _requestHandlers;
        size_t                                          _maxOutboxDepth{0}, _totalOutboxDepth{0}, _countOutboxDepth{0};
        uint64_t                                        _totalBytesWritten{0}, _totalBytesRead{0};
        Stopwatch                                       _timeOpen;
        atomic_flag                                     _connectedWebSocket = ATOMIC_FLAG_INIT;
        Retained<WeakHolder<Delegate>>                  _weakThis{new WeakHolder<Delegate>(this)};

      public:
        BLIPIO(Connection* connection, WebSocket* webSocket, Deflater::CompressionLevel compressionLevel)
            : Actor(BLIPLog, string("BLIP[") + connection->name() + "]")
            , _connection(connection)
            , _webSocket(webSocket)
            , _incomingFrames(this, "incomingFrames", &BLIPIO::_onWebSocketMessages)
            , _outbox(10)
            , _outputCodec(compressionLevel) {
            _pendingRequests.reserve(10);
            _pendingResponses.reserve(10);
        }

        std::string loggingClassName() const override { return "BLIPIO"; }

        void start() { enqueue(FUNCTION_TO_QUEUE(BLIPIO::_start)); }

        void terminate() {
            if ( !_connectedWebSocket.test_and_set() ) {
                _webSocket->close();
                _webSocket  = nullptr;
                _connection = nullptr;
                _weakThis   = nullptr;
            }
        }

        void queueMessage(MessageOut* msg) {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_queueMessage), Retained<MessageOut>(msg));
        }

        void setRequestHandler(std::string profile, bool atBeginning, Connection::RequestHandler handler) {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_setRequestHandler), std::move(profile), atBeginning, std::move(handler));
        }

        void close(CloseCode closeCode = kCodeNormal, slice message = nullslice) {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_close), closeCode, alloc_slice(message));
        }

        WebSocket* webSocket() const { return _webSocket; }

        std::string loggingIdentifier() const override {
            return _connection ? _connection->name()
                               : Logging::loggingIdentifier();  // NOLINT(bugprone-parent-virtual-call)
        }


      protected:
        ~BLIPIO() override {
            double outboxDepth =
                    (_countOutboxDepth != 0)
                            ? (static_cast<double>(_totalOutboxDepth) / static_cast<double>(_countOutboxDepth))
                            : 0.0;
            LogTo(SyncLog,
                  "BLIP sent %zu msgs (%" PRIu64 " bytes), rcvd %" PRIu64 " msgs (%" PRIu64
                  " bytes) in %.3f sec. Max outbox depth was %zu, avg %.2f",
                  _countOutboxDepth, _totalBytesWritten, _numRequestsReceived, _totalBytesRead, _timeOpen.elapsed(),
                  _maxOutboxDepth, outboxDepth);
            logStats();
        }

        void onWebSocketGotHTTPResponse(int status, const websocket::Headers& headers) override {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_gotHTTPResponse), status, headers);
        }

        virtual void onWebSocketGotTLSCertificate(slice certData) override {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_gotTLSCertificate), alloc_slice{certData});
        }

        // websocket::Delegate interface:
        void onWebSocketConnect() override {
            _timeOpen.reset();
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_onWebSocketConnect));
        }

        void onWebSocketClose(websocket::CloseStatus status) override {
            enqueue(FUNCTION_TO_QUEUE(BLIPIO::_closed), status);
        }

        void onWebSocketWriteable() override { enqueue(FUNCTION_TO_QUEUE(BLIPIO::_onWebSocketWriteable)); }

        void onWebSocketMessage(websocket::Message* message) override {
            if ( message->binary ) _incomingFrames.push(message);
            else
                warn("Ignoring non-binary WebSocket message");
        }

      private:
        void _start() {
            Assert(!_connectedWebSocket.test_and_set());
            retain(this);  // keep myself from being freed while I'm the webSocket's delegate
            _webSocket->connect(_weakThis);
        }

        void _gotHTTPResponse(int status, websocket::Headers headers) {
            // _connection is reset to nullptr in _closed.
            if ( _connection ) _connection->gotHTTPResponse(status, headers);
        }

        void _gotTLSCertificate(alloc_slice certData) {
            // _connection is reset to nullptr in _closed.
            if ( _connection ) _connection->gotTLSCertificate(certData);
        }

        void _onWebSocketConnect() {
            // _connection is reset to nullptr in _closed.
            if ( _connection ) {
                _connection->connected();
                _onWebSocketWriteable();
            }
        }

        /** Implementation of public close() method. Closes the WebSocket. */
        // Cannot use const& because it breaks Actor::enqueue
        void _close(CloseCode closeCode, alloc_slice message) {  // NOLINT(performance-unnecessary-value-param)
            if ( _webSocket && !_closingWithError ) { _webSocket->close(closeCode, message); }
        }

        void _closeWithError(const error& x) {
            if ( !_webSocket ) {
                warn("_closeWithError received error with null websocket");
            } else if ( _closingWithError ) {
                warn("_closeWithError called more than once (this time with %d / %d)", x.domain, x.code);
            } else {
                _webSocket->close(kCodeUnexpectedCondition, "Unexpected exception"_sl);
                _closingWithError = std::make_unique<error>(x);
            }
        }

        void _closed(websocket::CloseStatus status) {
            _onWebSocketMessages();  // process any pending incoming frames

            _webSocket = nullptr;
            if ( _connection ) {
                Retained<BLIPIO> holdOn(this);
                if ( _closingWithError ) {
                    status.reason  = kException;
                    status.code    = _closingWithError->code;
                    status.message = alloc_slice(_closingWithError->what());
                }
                _connection->closed(status);
                _connection = nullptr;
                cancelAll(_outbox);
                cancelAll(_icebox);
                cancelAll(_pendingRequests);
                cancelAll(_pendingResponses);
                _requestHandlers.clear();
                _weakThis = nullptr;
                release(this);  // webSocket is done calling delegate now (balances retain in ctor)
            } else {
                warn("_closed called on a null connection");
            }
        }

#pragma mark OUTGOING:

        /** Implementation of public queueMessage() method.
            Adds a new message to the outgoing queue and wakes up the queue. */
        // Cannot use const& because it breaks Actor::enqueue
        void _queueMessage(Retained<MessageOut> msg) {  // NOLINT(performance-unnecessary-value-param)
            if ( !_webSocket || _closingWithError ) {
                logInfo("Can't send %s #%" PRIu64 "; socket is closed", kMessageTypeNames[msg->type()], msg->number());
                msg->disconnected();
                return;
            }
            if ( BLIPLog.willLog(LogLevel::Verbose) ) {
                if ( !msg->isAck() || BLIPLog.willLog(LogLevel::Debug) )
                    logVerbose("Sending %s", msg->description().c_str());
            }
            _maxOutboxDepth = max(_maxOutboxDepth, _outbox.size() + 1);
            _totalOutboxDepth += _outbox.size() + 1;
            ++_countOutboxDepth;
            requeue(msg, true);
        }

        /** Adds a message to the outgoing queue */
        void requeue(MessageOut* msg, bool andWrite = false) {
            DebugAssert(!_outbox.contains(msg));
            auto i = _outbox.end();
            if ( msg->urgent() && _outbox.size() > 1 ) {
                // High-priority gets queued after the last existing high-priority message,
                // leaving one regular-priority message in between if possible:
                const bool isNew = (msg->_bytesSent == 0);
                do {
                    --i;
                    if ( (*i)->urgent() ) {
                        if ( (i + 1) != _outbox.end() ) ++i;
                        break;
                    } else if ( isNew && (*i)->_bytesSent == 0 ) {
                        break;
                    }
                } while ( i != _outbox.begin() );
                ++i;
            }
            logVerbose("Requeuing %s #%" PRIu64 "...", kMessageTypeNames[msg->type()], msg->number());
            _outbox.emplace(i, msg);  // inserts _at_ position i, before message *i

            if ( andWrite ) writeToWebSocket();
        }

        /** Adds an outgoing message to the icebox (until an ACK arrives.) */
        void freezeMessage(MessageOut* msg) {
            logVerbose("Freezing %s #%" PRIu64 "", kMessageTypeNames[msg->type()], msg->number());
            DebugAssert(!_outbox.contains(msg));
            DebugAssert(!_icebox.contains(msg));
            _icebox.push_back(msg);
        }

        /** Removes an outgoing message from the icebox and re-queues it (after ACK arrives.) */
        void thawMessage(MessageOut* msg) {
            logVerbose("Thawing %s #%" PRIu64 "", kMessageTypeNames[msg->type()], msg->number());
            LITECORE_UNUSED bool removed = _icebox.remove(msg);
            DebugAssert(removed);
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
            if ( !_writeable ) {
                warn("writeToWebSocket cannot start, unwritable!");
                return;
            }

            size_t bytesWritten = 0;
            logVerbose("Starting writeToWebSocket loop...");
            while ( _writeable ) {
                // Get the next message, if any, from the queue:
                Retained<MessageOut> msg(_outbox.pop());
                if ( !msg ) break;

                // Assign the message number for new requests.
                if ( msg->_number == 0 ) msg->_number = ++_lastMessageNo;

                FrameFlags frameFlags;
                {
                    // Set up a buffer for the frame contents:
                    size_t maxSize = kDefaultFrameSize;
                    if ( msg->urgent() || _outbox.empty() || !_outbox.front()->urgent() ) maxSize = kBigFrameSize;

                    if ( !_frameBuf ) _frameBuf.reset(new uint8_t[kMaxVarintLen64 + 1 + 4 + kBigFrameSize]);
                    slice_ostream out(_frameBuf.get(), maxSize);
                    out.writeUVarInt(msg->_number);
                    auto flagsPos = (FrameFlags*)out.next();
                    out.advance(1);

                    // Ask the MessageOut to write data to fill the buffer:
                    auto prevBytesSent = msg->_bytesSent;
                    msg->nextFrameToSend(_outputCodec, out, frameFlags);
                    *flagsPos   = frameFlags;
                    slice frame = out.output();
                    bytesWritten += frame.size;

                    logVerbose("    Sending frame: %s #%" PRIu64 " %c%c%c%c, bytes %u--%u",
                               kMessageTypeNames[frameFlags & kTypeMask], msg->number(),
                               (frameFlags & kMoreComing ? 'M' : '-'), (frameFlags & kUrgent ? 'U' : '-'),
                               (frameFlags & kNoReply ? 'N' : '-'), (frameFlags & kCompressed ? 'C' : '-'),
                               prevBytesSent, msg->_bytesSent - 1);
                    //logVerbose("    %s", frame.hexString().c_str());
                    // Write it to the WebSocket:
                    _writeable = _webSocket->send(frame);
                }

                // Return message to the queue if it has more frames left to send:
                if ( frameFlags & kMoreComing ) {
                    if ( msg->needsAck() ) freezeMessage(msg);
                    else
                        requeue(msg);
                } else {
                    if ( !msg->isAck() ) {
                        logVerbose("Finished sending %s", msg->description().c_str());
                        // Add its response message to _pendingResponses:
                        MessageIn* response = msg->createResponse();
                        if ( response ) _pendingResponses.emplace(response->number(), response);
                    }
                }
            }
            _totalBytesWritten += bytesWritten;
            logVerbose("...Wrote %zu bytes to WebSocket (writeable=%d)", bytesWritten, _writeable);
        }

#pragma mark INCOMING:

        /** WebSocketDelegate method -- Received a frame: */
        void _onWebSocketMessages(int gen = actor::AnyGen) {
            auto messages = _incomingFrames.pop(gen);
            if ( !messages ) {
                if ( gen != actor::AnyGen ) { warn("onWebSocketMessages couldn't find any messages to process"); }
                return;
            }

            try {
                for ( auto& wsMessage : *messages ) {
                    if ( _closingWithError ) {
                        warn("Cancelling onWebSocketMessages loop due to closing with error");
                        return;
                    }

                    // Read the frame header:
                    slice_istream payload = wsMessage->data;
                    _totalBytesRead += payload.size;
                    uint64_t   msgNo;
                    FrameFlags flags;
                    {
                        auto pMsgNo = payload.readUVarInt(), pFlags = payload.readUVarInt();
                        if ( !pMsgNo || !pFlags ) throw runtime_error("Illegal BLIP frame header");
                        msgNo = *pMsgNo;
                        flags = (FrameFlags)*pFlags;
                    }
                    logVerbose("Received frame: %s #%" PRIu64 " %c%c%c%c, length %5ld",
                               kMessageTypeNames[flags & kTypeMask], msgNo, (flags & kMoreComing ? 'M' : '-'),
                               (flags & kUrgent ? 'U' : '-'), (flags & kNoReply ? 'N' : '-'),
                               (flags & kCompressed ? 'C' : '-'), (long)payload.size);

                    // Handle the frame according to its type, and look up the MessageIn:
                    Retained<MessageIn> msg;
                    auto                type = (MessageType)(flags & kTypeMask);
                    switch ( type ) {
                        case kRequestType:
                            msg = pendingRequest(msgNo, flags);
                            break;
                        case kResponseType:
                        case kErrorType:
                            {
                                msg = pendingResponse(msgNo, flags);
                                break;
                                case kAckRequestType:
                                case kAckResponseType:
                                    receivedAck(msgNo, (type == kAckResponseType), payload);
                                    break;
                                default:
                                    warn("Unknown BLIP frame type received");
                                    // For forward compatibility let's just ignore this instead of closing
                                    break;
                            }
                    }

                    // Append the frame to the message:
                    if ( msg ) {
                        MessageIn::ReceiveState state;
                        try {
                            state = msg->receivedFrame(_inputCodec, payload, flags);
                        } catch ( ... ) {
                            // If this is the final frame, then msg may not be in either pending list
                            // anymore. But on an exception we need to call its progress handler to
                            // disconnect it, so make sure to re-add it:
                            if ( type == kRequestType ) _pendingRequests.emplace(msgNo, msg);
                            else if ( type == kResponseType )
                                _pendingResponses.emplace(msgNo, msg);
                            throw;
                        }

                        if ( state == MessageIn::kEnd ) {
                            if ( BLIPMessagesLog.willLog(LogLevel::Info) ) {
                                stringstream dump;
                                bool         withBody = BLIPMessagesLog.willLog(LogLevel::Verbose);
                                msg->dump(dump, withBody);
                                BLIPMessagesLog.log(LogLevel::Info, "RECEIVED: %s", dump.str().c_str());
                            }
                        }

                        if ( type == kRequestType ) {
                            if ( state == MessageIn::kEnd || state == MessageIn::kBeginning ) {
                                // Message complete!
                                handleRequestReceived(msg, state);
                            }
                        }
                    }

                    wsMessage = nullptr;  // free the frame
                }

            } catch ( const std::exception& x ) {
                logError("Caught exception handling incoming BLIP message: %s", x.what());
                _closeWithError(error::convertException(x));
            }
        }

        /** Handle an incoming ACK message, by unfreezing the associated outgoing message. */
        void receivedAck(MessageNo msgNo, bool onResponse, slice body) {
            // Find the MessageOut in either _outbox or _icebox:
            bool                 frozen = false;
            Retained<MessageOut> msg    = _outbox.findMessage(msgNo, onResponse);
            if ( !msg ) {
                msg = _icebox.findMessage(msgNo, onResponse);
                if ( !msg ) {
                    logVerbose("Received ACK of non-current message (%s #%" PRIu64 ")", (onResponse ? "RES" : "REQ"),
                               msgNo);
                    return;
                }
                frozen = true;
            }

            // Acks have no checksum and don't go through the codec; just read the byte count:
            auto byteCount = slice_istream(body).readUVarInt32();
            if ( !byteCount ) {
                warn("Couldn't parse body of ACK");
                return;
            }

            msg->receivedAck(*byteCount);
            if ( frozen && !msg->needsAck() ) thawMessage(msg);
        }

        /** Returns the MessageIn object for the incoming request with the given MessageNo. */
        Retained<MessageIn> pendingRequest(MessageNo msgNo, FrameFlags flags) {
            Retained<MessageIn> msg;
            auto                i = _pendingRequests.find(msgNo);
            if ( i != _pendingRequests.end() ) {
                // Existing request: return it, and remove from _pendingRequests if the last frame:
                msg = i->second;
                if ( !(flags & kMoreComing) ) {
                    logInfo("REQ #%" PRIu64 " has reached the end of its frames", msgNo);
                    _pendingRequests.erase(i);
                }
            } else if ( msgNo == _numRequestsReceived + 1 ) {
                // New request: create and add to _pendingRequests unless it's a singleton frame:
                ++_numRequestsReceived;
                msg = new MessageIn(_connection, flags, msgNo);
                if ( flags & kMoreComing ) {
                    _pendingRequests.emplace(msgNo, msg);
                    logInfo("REQ #%" PRIu64 " has more frames coming", msgNo);
                }
            } else {
                throw runtime_error(format("BLIP protocol error: Bad incoming REQ #%" PRIu64 " (%s)", msgNo,
                                           (msgNo <= _numRequestsReceived ? "already finished" : "too high")));
            }
            return msg;
        }

        /** Returns the MessageIn object for the incoming response with the given MessageNo. */
        Retained<MessageIn> pendingResponse(MessageNo msgNo, FrameFlags flags) {
            Retained<MessageIn> msg;
            auto                i = _pendingResponses.find(msgNo);
            if ( i != _pendingResponses.end() ) {
                msg = i->second;
                if ( !(flags & kMoreComing) ) {
                    logVerbose("RES #%" PRIu64 " has reached the end of its frames", msgNo);
                    _pendingResponses.erase(i);
                }
            } else {
                throw runtime_error(format("BLIP protocol error: Bad incoming RES #%" PRIu64 " (%s)", msgNo,
                                           (msgNo <= _lastMessageNo ? "no request waiting" : "too high")));
            }
            return msg;
        }

        void cancelAll(MessageQueue& queue) {  // either _outbox or _icebox
            if ( !queue.empty() ) logInfo("Notifying %zd outgoing messages they're canceled", queue.size());
            for ( auto& msg : queue ) msg->disconnected();
            queue.clear();
        }

        void cancelAll(MessageMap& pending) {  // either _pendingResponses or _pendingRequests
            if ( !pending.empty() ) logInfo("Notifying %zd incoming messages they're canceled", pending.size());
            for ( auto& item : pending ) item.second->disconnected();
            pending.clear();
        }

        // NOLINTBEGIN(performance-unnecessary-value-param)
        // Cannot use const& because it breaks Actor::enqueue
        void _setRequestHandler(std::string profile, bool atBeginning, Connection::RequestHandler handler) {
            HandlerKey key{profile, atBeginning};
            if ( handler ) _requestHandlers.emplace(key, handler);
            else
                _requestHandlers.erase(key);
        }

        // NOLINTEND(performance-unnecessary-value-param)

        void handleRequestBeginning(MessageIn* request) {}

        void handleRequestReceived(MessageIn* request, MessageIn::ReceiveState state) {
            try {
                if ( state == MessageIn::kOther ) {
                    warn("handleRequestReceived received a message in a suspicious state (kOther)");
                    return;
                }

                bool beginning = (state == MessageIn::kBeginning);
                auto profile   = request->property("Profile"_sl);
                if ( profile ) {
                    auto i = _requestHandlers.find({profile.asString(), beginning});
                    if ( i != _requestHandlers.end() ) {
                        i->second(request);
                        return;
                    }
                }

                logInfo("No handler for profile '%.*s', falling back to delegate callbacks", SPLAT(profile));
                if ( beginning ) _connection->delegateWeak()->invoke(&ConnectionDelegate::onRequestBeginning, request);
                else
                    _connection->delegateWeak()->invoke(&ConnectionDelegate::onRequestReceived, request);
            } catch ( ... ) {
                logError("Caught exception thrown from BLIP request handler");
                request->respondWithError({"BLIP"_sl, 501, "unexpected exception"_sl});
            }
        }

    };  // end of class BLIPIO

#pragma mark - CONNECTION:

    Connection::Connection(WebSocket* webSocket, const fleece::AllocedDict& options,
                           Retained<WeakHolder<ConnectionDelegate>> weakDelegate)
        : Logging(BLIPLog), _name(webSocket->name()), _role(webSocket->role()), _weakDelegate(std::move(weakDelegate)) {
        if ( _role == Role::Server ) logInfo("Accepted connection");
        else
            logInfo("Opening connection...");

        _compressionLevel = kDefaultCompressionLevel;
        auto levelP       = options.get(kCompressionLevelOption);
        if ( levelP.isInteger() ) _compressionLevel = (int8_t)levelP.asInt();

        // Now connect the websocket:
        _io = new BLIPIO(this, webSocket, (Deflater::CompressionLevel)_compressionLevel);
        _io->setParentObjectRef(getObjectRef());
        websocket::WebSocketImpl* logging = dynamic_cast<websocket::WebSocketImpl*>(webSocket);
        if ( logging ) { logging->setParentObjectRef(getObjectRef()); }
    }

    Connection::~Connection() { logDebug("~Connection"); }

    void Connection::start(Retained<WeakHolder<blip::ConnectionDelegate>> connectionDelegate) {
        // Can't use std::move here because it breaks the Retain
        _weakDelegate = connectionDelegate;  //NOLINT(performance-unnecessary-value-param)
        Assert(_state == kClosed);
        _state = kConnecting;
        _io->start();
    }

    /** Public API to send a new request. */
    void Connection::sendRequest(MessageBuilder& mb) {
        Retained<MessageOut> message = new MessageOut(this, mb, 0);
        DebugAssert(message->type() == kRequestType);
        send(message);
    }

    /** Internal API to send an outgoing message (a request, response, or ACK.) */
    void Connection::send(MessageOut* msg) {
        if ( _compressionLevel == 0 ) msg->dontCompress();
        if ( BLIPMessagesLog.effectiveLevel() <= LogLevel::Info ) {
            stringstream dump;
            bool         withBody = BLIPMessagesLog.willLog(LogLevel::Verbose);
            msg->dump(dump, withBody);
            BLIPMessagesLog.log(LogLevel::Info, "SENDING: %s", dump.str().c_str());
        }

        _io->queueMessage(msg);
    }

    void Connection::setRequestHandler(string profile, bool atBeginning, RequestHandler handler) {
        _io->setRequestHandler(std::move(profile), atBeginning, std::move(handler));
    }

    void Connection::gotHTTPResponse(int status, const websocket::Headers& headers) {
        delegateWeak()->invoke(&ConnectionDelegate::onHTTPResponse, status, headers);
    }

    void Connection::gotTLSCertificate(slice certData) {
        delegateWeak()->invoke(&ConnectionDelegate::onTLSCertificate, certData);
    }

    void Connection::connected() {
        logInfo("Connected!");
        _state = kConnected;
        delegateWeak()->invoke(&ConnectionDelegate::onConnect);
    }

    void Connection::close(CloseCode closeCode, slice errorMessage) {
        logInfo("Closing with code %d, msg '%.*s'", closeCode, SPLAT(errorMessage));
        _state = kClosing;
        _io->close(closeCode, errorMessage);
    }

    void Connection::closed(const CloseStatus& status) {
        logInfo("Closed with %-s %d: %.*s", status.reasonName(), status.code, SPLAT(status.message));
        _state       = status.isNormal() ? kClosed : kDisconnected;
        _closeStatus = status;
        delegateWeak()->invoke(&ConnectionDelegate::onClose, status, _state);
    }

    void Connection::terminate() {
        Assert(_state == kClosed);
        _io->terminate();
        _io = nullptr;
    }

    websocket::WebSocket* Connection::webSocket() const { return _io->webSocket(); }

}  // namespace litecore::blip
