//
// WebSocketImpl.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include "NumConversion.hh"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <thread>
#include <utility>
#include <vector>
struct C4Socket;

namespace c4SocketTrace {
    using namespace std;

    struct Event {
        C4Socket*  socket;
        int64_t    timestamp;
        thread::id tid;
        string     func;
        string     remark;

        Event(const C4Socket* sock, const string& f);
        Event(const C4Socket* sock, const string& f, const string& rem);

        explicit operator string();
    };

    class EventQueue : public vector<Event> {
      public:
        void addEvent(const C4Socket* sock, const string& f);
        void addEvent(const C4Socket* sock, const string& f, const string& rem);

      private:
        mutex mut;
    };

    EventQueue& traces();
}  // namespace c4SocketTrace

using namespace std;
using namespace uWS;
using namespace fleece;

namespace litecore::websocket {

    static constexpr size_t kSendBufferSize = 64 * 1024;

    // Default interval at which to send PING messages (configurable via options)
    static constexpr auto kDefaultHeartbeatInterval = chrono::seconds(5 * 60);

    // Timeout for disconnecting if no PONG response received
    static constexpr auto kPongTimeout = 10s;

    // Timeout for disconnecting if no CLOSE response received
    static constexpr auto kCloseTimeout = 5s;

    class MessageImpl : public Message {
      public:
        MessageImpl(WebSocketImpl* ws, slice data, bool binary)
            : Message(data, binary), _size(data.size), _webSocket(ws) {}

        ~MessageImpl() override { _webSocket->receiveComplete(_size); }

      private:
        size_t const         _size;
        WebSocketImpl* const _webSocket;
    };

    WebSocketImpl::WebSocketImpl(const URL& url, Role role, bool framing, Parameters parameters)
        : WebSocket(url, role)
        , Logging(WSLogDomain)
        , _parameters(std::move(parameters))
        , _framing(framing)
        , _responseTimer(new actor::Timer([this] { timedOut(); })) {
        if ( framing ) {
            if ( role == Role::Server ) _serverProtocol = std::make_unique<ServerProtocol>();
            else
                _clientProtocol = std::make_unique<ClientProtocol>();
        }
    }

    WebSocketImpl::~WebSocketImpl() = default;

    string WebSocketImpl::loggingIdentifier() const { return string(url()); }

    void WebSocketImpl::connect() {
        logInfo("Connecting...");
        _socketLCState.store(SOCKET_OPENING);
        startResponseTimer(chrono::seconds(kConnectTimeoutSecs));
    }

    void WebSocketImpl::gotHTTPResponse(int status, const websocket::Headers& headersFleece) {
        logInfo("Got HTTP response (status %d)", status);
        delegateWeak()->invoke(&Delegate::onWebSocketGotHTTPResponse, status, headersFleece);
    }

    void WebSocketImpl::onConnect() {
        int expected = SOCKET_OPENING;
        if ( !atomic_compare_exchange_strong(&_socketLCState, &expected, (int)SOCKET_OPENED) ) {
            logInfo("WebSocket not in 'Openning' state, ignoring onConnect...");
            return;
        }

        logInfo("Connected!");
        _didConnect = true;
        _responseTimer->stop();
        _timeConnected.start();
        delegateWeak()->invoke(&Delegate::onWebSocketConnect);

        // Initialize ping timer. (This is the first time it's accessed, and this method is only
        // called once, so no locking is needed.)
        if ( _framing ) {
            if ( heartbeatInterval() > 0 ) {
                logVerbose("Setting ping timer to %d...", heartbeatInterval());
                _pingTimer = std::make_unique<actor::Timer>([this] { sendPing(); });
                schedulePing();
            }
        }
    }

    bool WebSocketImpl::send(fleece::slice message, bool binary) {
        logVerbose("Sending %zu-byte message", message.size);
        return sendOp(message, binary ? uWS::BINARY : uWS::TEXT);
    }

    bool WebSocketImpl::sendOp(fleece::slice message, uint8_t opcode) {
        alloc_slice frame;
        bool        writeable;
        {
            lock_guard<std::mutex> lock(_mutex);
            if ( _closeSent && opcode != CLOSE ) {
                warn("sendOp refusing to send msg type %d after close", opcode);
                return false;
            }

            if ( _framing ) {
                frame.resize(message.size + 10);  // maximum space needed
                size_t newSize;
                if ( role() == Role::Server ) {
                    newSize = ServerProtocol::formatMessage((std::byte*)frame.buf, (const char*)message.buf,
                                                            message.size, (uWS::OpCode)opcode, message.size, false);
                } else {
                    newSize = ClientProtocol::formatMessage((std::byte*)frame.buf, (const char*)message.buf,
                                                            message.size, (uWS::OpCode)opcode, message.size, false);
                }
                frame.shorten(newSize);
            } else {
                DebugAssert(opcode == uWS::BINARY);
                frame = message;
            }
            _bufferedBytes += frame.size;
            writeable = (_bufferedBytes <= kSendBufferSize);
        }
        // Release the lock before calling sendBytes, because that's an abstract method, and some
        // implementation of it might call back into me and deadlock.
        sendBytes(frame);
        return writeable;
    }

    void WebSocketImpl::onWriteComplete(size_t size) {
        bool notify, disconnect;
        {
            lock_guard<mutex> lock(_mutex);
            _bytesSent += size;
            notify = (_bufferedBytes > kSendBufferSize);
            _bufferedBytes -= size;
            if ( _bufferedBytes > kSendBufferSize ) notify = false;

            disconnect = (_closeSent && _closeReceived && _bufferedBytes == 0);
        }

        if ( disconnect ) {
            // My close message has gone through; now I can disconnect:
            logInfo("sent close echo; disconnecting socket now");
            callCloseSocket();
        } else if ( notify ) {
            delegateWeak()->invoke(&Delegate::onWebSocketWriteable);
        }
    }

    void WebSocketImpl::onReceive(slice data) {
        ssize_t     completedBytes = 0;
        uint8_t     opToSend       = 0;
        alloc_slice msgToSend;
        {
            // Lock the mutex; this protects all methods (below) involved in receiving,
            // since they're called from this one.
            lock_guard<mutex> lock(_mutex);

            if ( data.empty() && !_closeReceived ) {
                // We assume empty data means a zero-length read, i.e. EOF
                logError("Protocol error: Peer shutdown socket without a CLOSE message");
                protocolError("Peer shutdown socket without a CLOSE message"_sl);
                return;
            }

            _bytesReceived += data.size;
            if ( _framing ) {
                _deliveredBytes          = 0;
                size_t prevMessageLength = _curMessageLength;
                // this next line will call handleFragment(), below --
                if ( _clientProtocol ) _clientProtocol->consume((std::byte*)data.buf, data.size, this);
                else
                    _serverProtocol->consume((std::byte*)data.buf, data.size, this);
                opToSend  = _opToSend;
                msgToSend = std::move(_msgToSend);
                // Compute # of bytes consumed: just the framing data, not any partial or
                // delivered messages. (Trust me, the math works.)
                completedBytes =
                        narrow_cast<ssize_t>(data.size + prevMessageLength - _curMessageLength - _deliveredBytes);
            }
        }
        if ( !_framing ) deliverMessageToDelegate(data, true);

        if ( completedBytes > 0 ) receiveComplete(completedBytes);

        // Send any message that was generated during the locked block above:
        if ( msgToSend ) sendOp(msgToSend, opToSend);
    }

    // Called from inside _protocol->consume(), with the _mutex locked
    bool WebSocketImpl::handleFragment(std::byte* data, size_t length, size_t remainingBytes, uint8_t opCode,
                                       bool fin) {
        // Beginning:
        if ( !_curMessage ) {
            _curOpCode = opCode;
            _curMessage.reset(length + remainingBytes);
            _curMessageLength = 0;
        }

        // Body:
        if ( _curMessageLength + length > _curMessage.size ) {
            // We tried...but there is still more data, so resize
            _curMessage.resize(_curMessageLength + length);
        }

        // CBL-2169: addressing the 0-th element of 0-length slice will trigger assertion failure.
        if ( length > 0 ) {
            memcpy((void*)&_curMessage[_curMessageLength], data, length);
            _curMessageLength += length;
        }

        // End:
        if ( fin && remainingBytes == 0 ) {
            _curMessage.shorten(_curMessageLength);
            bool ok     = receivedMessage(_curOpCode, _curMessage);
            _curMessage = nullptr;
            DebugAssert(!_curMessage);
            _curMessageLength = 0;
            return ok;
        }
        return true;
    }

    // Called from handleFragment, with the mutex locked
    bool WebSocketImpl::receivedMessage(uint8_t opCode, const alloc_slice& message) {
        switch ( opCode ) {
            case TEXT:
                if ( !ClientProtocol::isValidUtf8((unsigned char*)message.buf, message.size) ) return false;
                // fall through:
            case BINARY:
                deliverMessageToDelegate(message, (opCode == BINARY));
                return true;
            case CLOSE:
                return receivedClose(message);
            case PING:
                _opToSend  = PONG;
                _msgToSend = message ? message : alloc_slice(size_t(0));
                return true;
            case PONG:
                receivedPong();
                return true;
            default:
                return false;
        }
    }

    // Called from inside _protocol->consume(), with the _mutex locked
    void WebSocketImpl::protocolError(slice message) {
        _protocolError = message;
        callCloseSocket();
    }

    void WebSocketImpl::deliverMessageToDelegate(slice data, C4UNUSED bool binary) {
        logVerbose("Received %zu-byte message", data.size);
        _deliveredBytes += data.size;
        Retained<Message> message(new MessageImpl(this, data, true));
        delegateWeak()->invoke(&Delegate::onWebSocketMessage, message);
    }

#pragma mark - HEARTBEAT:

    int WebSocketImpl::heartbeatInterval() const {
        if ( !_framing ) return 0;
        else if ( _parameters.heartbeatSecs > 0 )
            return _parameters.heartbeatSecs;
        else
            return (int)kDefaultHeartbeatInterval.count();
    }

    void WebSocketImpl::schedulePing() {
        if ( !_closeSent ) _pingTimer->fireAfter(chrono::seconds(heartbeatInterval()));
    }

    // timer callback
    void WebSocketImpl::sendPing() {
        {
            lock_guard<mutex> lock(_mutex);
            if ( !_pingTimer ) {
                warn("Ping timer not available, giving up on sendPing...");
                return;
            }

            schedulePing();
            startResponseTimer(kPongTimeout);
            // exit scope to release the lock -- this is needed before calling sendOp,
            // which acquires the lock itself
        }
        logInfo("Sending PING");
        sendOp(nullslice, PING);
    }

    void WebSocketImpl::receivedPong() {
        logInfo("Received PONG");
        _responseTimer->stop();
    }

    void WebSocketImpl::startResponseTimer(chrono::seconds timeoutSecs) {
        _curTimeout = timeoutSecs;
        if ( _responseTimer ) _responseTimer->fireAfter(timeoutSecs);
    }

    void WebSocketImpl::timedOut() {
        logError("No response received after %lld sec -- disconnecting", (long long)_curTimeout.count());
        _timedOut = true;
        switch ( _socketLCState.load() ) {
            case SOCKET_OPENING:
            case SOCKET_OPENED:
                if ( _framing ) callCloseSocket();
                else
                    callRequestClose(504, "Timed out"_sl);
                break;
            case SOCKET_CLOSING:
                {
                    CloseStatus status = {kNetworkError, kNetErrTimeout, nullslice};
                    onClose(status);
                }
                break;
            default:
                break;
        }
    }

#pragma mark - CLOSING:

    // See <https://tools.ietf.org/html/rfc6455#section-7>


    void WebSocketImpl::callCloseSocket() {
        int expected[] = {SOCKET_OPENING, SOCKET_OPENED};
        int i          = 0;
        for ( ; i < 2; ++i ) {
            if ( atomic_compare_exchange_strong(&_socketLCState, &expected[i], (int)SOCKET_CLOSING) ) {
                if ( i == 0 ) { logVerbose("Calling closeSocket before the socket is connected"); }
                // else: This is the usual case: from OPENED to CLOSING
                break;
            }
        }
        if ( i < 2 ) {
            startResponseTimer(kCloseTimeout);
            closeSocket();
        } else {
            logVerbose("Calling closeSocket when the socket is %s",
                       expected[1] == SOCKET_CLOSING ? "pending close" : "already closed");
        }
    }

    void WebSocketImpl::callRequestClose(int status, fleece::slice message) {
        int expected[] = {SOCKET_OPENING, SOCKET_OPENED};
        int i          = 0;
        for ( ; i < 2; ++i ) {
            if ( atomic_compare_exchange_strong(&_socketLCState, &expected[i], (int)SOCKET_CLOSING) ) {
                if ( i == 0 ) { logVerbose("Calling requestClose before the socket is connected"); }
                // else: This is the usual case: from OPENED to CLOSING
                break;
            }
        }
        if ( i < 2 ) {
            startResponseTimer(kCloseTimeout);
            requestClose(status, message);
        } else {
            logVerbose("Calling requestClose when the socket is %s",
                       expected[1] == SOCKET_CLOSING ? "pending close" : "is already closed");
        }
    }

    // Initiates a request to close the connection cleanly.
    void WebSocketImpl::close(int status, fleece::slice message) {
        int currState = SOCKET_UNINIT;
        switch ( _socketLCState.load() ) {
            case SOCKET_CLOSING:
                logVerbose("Calling close when the socket is pending close");
                return;
            case SOCKET_CLOSED:
                logVerbose("Calling close when the socket is already closed");
                return;
            case SOCKET_OPENED:
                currState = SOCKET_OPENED;
                logInfo("Requesting close with status=%d, message='%.*s'", status, SPLAT(message));
                if ( _framing ) {
                    alloc_slice closeMsg;
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        if ( _closeSent || _closeReceived ) {
                            logVerbose("Close already processed (_closeSent: %d, _closeReceived: %d), exiting "
                                       "WebSocketImpl::close()",
                                       (int)_closeSent, (int)_closeReceived);
                            return;
                        }

                        closeMsg  = alloc_slice(2 + message.size);
                        auto size = ClientProtocol::formatClosePayload((std::byte*)closeMsg.buf, (uint16_t)status,
                                                                       (const char*)message.buf, message.size);
                        closeMsg.shorten(size);
                        _closeSent    = true;
                        _closeMessage = closeMsg;
                        startResponseTimer(kCloseTimeout);
                    }
                    sendOp(closeMsg, uWS::CLOSE);
                    return;
                }
            case SOCKET_OPENING:
                if ( currState != SOCKET_OPENED ) { logVerbose("Calling close before the socket is connected"); }
                if ( _framing ) {
                    logInfo("Closing socket before connection established...");
                    // The web socket is being requested to close before it's even connected, so just
                    // shortcut to the callback and make sure that onConnect does nothing now
                    callCloseSocket();
                } else {
                    callRequestClose(status, message);
                }
                return;
            case SOCKET_UNINIT:
                return;
            default:
                DebugAssert(false);
        }
    }

    // Handles a close message received from the peer. (Mutex is locked!)
    bool WebSocketImpl::receivedClose(slice message) {
        if ( _closeReceived ) return false;
        _closeReceived = true;
        if ( _closeSent ) {
            // I initiated the close; the peer has confirmed, so disconnect the socket now:
            logInfo("Close confirmed by peer; disconnecting socket now");
            callCloseSocket();
        } else {
            // Peer is initiating a close. Save its message and echo it:
            if ( willLog() ) {
                auto close = ClientProtocol::parseClosePayload((std::byte*)message.buf, message.size);
                logInfo("Client is requesting close (%d '%.*s'); echoing it", close.code, (int)close.length,
                        (char*)close.message);
            }
            _closeSent    = true;
            _closeMessage = message;
            // Don't send the message now or I'll deadlock; remember to do it later in onReceive:
            _msgToSend = message;
            _opToSend  = CLOSE;
        }
        _pingTimer.reset();
        _responseTimer.reset();
        return true;
    }

    void WebSocketImpl::onCloseRequested(int status, fleece::slice message) {
        DebugAssert(!_framing);
        callRequestClose(status, message);
    }

    void WebSocketImpl::onClose(int posixErrno) {
        alloc_slice message;
        if ( posixErrno ) message = slice(strerror(posixErrno));
        onClose({kPOSIXError, posixErrno, message});
    }

    // Called when the underlying socket closes.
    void WebSocketImpl::onClose(CloseStatus status) {
        switch ( atomic_exchange(&_socketLCState, (int)SOCKET_CLOSED) ) {
            case SOCKET_OPENING:
                logVerbose("Calling onClose before the socket is connected");
                break;
            case SOCKET_OPENED:
                logVerbose("Calling onClose before calling closeSocket/requestClose");
                break;
            case SOCKET_CLOSING:
                // The usual case: CLOSING -> CLOSED
                break;
            case SOCKET_CLOSED:
                logVerbose("Calling of onClose is ignored because it is already called.");
                return;
            default:
                DebugAssert(false);
        }

        auto logErrorForStatus = [this](const char* msg, const CloseStatus& cstatus) {
            if ( cstatus.message.empty() ) {
                logError("%s (reason=%-s %d)", msg, cstatus.reasonName(), cstatus.code);
            } else {
                logError("%s (reason=%-s %d) %.*s", msg, cstatus.reasonName(), cstatus.code, SPLAT(cstatus.message));
            }
        };

        {
            lock_guard<mutex> lock(_mutex);

            _pingTimer.reset();

            if ( !_timedOut ) {
                // CBL-2410: If _timedOut is true then we are almost
                // certainly in this method synchronously from the _responseTimer
                // callback which means resetting here would cause a hang.  Since
                // _timedOut is true, this timer has already fired anyway so there
                // is no pressing need for a tear down here, it can wait until later.
                _responseTimer.reset();
            }

            if ( status.reason == kWebSocketClose ) {
                if ( _timedOut ) status = {kNetworkError, kNetErrTimeout, nullslice};
                else if ( _protocolError ) {
                    status = {kWebSocketClose, kCodeProtocolError, _protocolError};
                    logErrorForStatus("WebSocketImpl::onClose", status);
                }
            }

            if ( _didConnect ) {
                bool clean = status.code == 0
                             || (status.reason == kWebSocketClose
                                 && (status.code == kCodeNormal || status.code == kCodeGoingAway));
                if ( _framing ) {
                    bool expected = (_closeSent && _closeReceived);
                    if ( expected && clean ) logInfo("Socket disconnected cleanly");
                    else {
                        std::stringstream ss;
                        ss << "Unexpected or unclean socket disconnect!";
                        if ( !_closeSent ) { ss << " (close not sent"; }
                        if ( !_closeReceived ) {
                            ss << (_closeSent ? " (" : "; ");
                            ss << "close not received)";
                        } else if ( !_closeSent ) {
                            ss << ")";
                        }
                        logErrorForStatus(ss.str().c_str(), status);
                    }

                    if ( clean ) {
                        status.reason = kWebSocketClose;
                        if ( !expected ) status.code = kCodeAbnormal;
                        else if ( !_closeMessage )
                            status.code = kCodeNormal;
                        else {
                            auto msg       = ClientProtocol::parseClosePayload((std::byte*)_closeMessage.buf,
                                                                               _closeMessage.size);
                            status.code    = msg.code ? msg.code : kCodeStatusCodeExpected;
                            status.message = slice(msg.message, msg.length);
                        }
                    }
                    _closeMessage = nullslice;
                } else {
                    if ( clean ) logInfo("WebSocket closed normally");
                    else
                        logErrorForStatus("WebSocket closed abnormally", status);
                }

                _timeConnected.stop();
                double t = _timeConnected.elapsed();
                // Our formater in LogEncoder does not recognize %Lf
                logInfo("sent %" PRIu64 " bytes, rcvd %" PRIu64 ", in %.3f sec (%.0f/sec, %.0f/sec)", _bytesSent,
                        _bytesReceived, t, (double)((long double)_bytesSent / t),
                        (double)((long double)_bytesReceived / t));
            } else {
                logErrorForStatus("WebSocket failed to connect!", status);
            }
        }
        delegateWeak()->invoke(&Delegate::onWebSocketClose, status);
    }

}  // namespace litecore::websocket

#pragma mark - WEBSOCKETPROTOCOL

// The rest of the implementation of uWS::WebSocketProtocol, which calls into WebSocket:
namespace uWS {

    static constexpr size_t kMaxMessageLength = 1 << 20;


// The `user` parameter points to the owning WebSocketImpl object.
#define USER_SOCK ((litecore::websocket::WebSocketImpl*)user)

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::setCompressed(C4UNUSED void* user) {
        return false;  //TODO: Implement compression
    }

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::refusePayloadLength(C4UNUSED void* user, size_t length) {
        return length > kMaxMessageLength;
    }

    template <const bool isServer>
    void WebSocketProtocol<isServer>::forceClose(void* user, const char* reason) {
        std::stringstream ss;
        ss << "WebSocketProtocol<" << (isServer ? "server" : "client") << ">::forceClose";
        if ( reason != nullptr ) { ss << reason; }
        USER_SOCK->logError("Protocol error: %s", ss.str().c_str());
        USER_SOCK->protocolError(slice(ss.str().c_str()));
    }

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::handleFragment(std::byte* data, size_t length, size_t remainingByteCount,
                                                     uint8_t opcode, bool fin, void* user) {
        // WebSocketProtocol expects this method to return true on error, but this confuses me
        // so I'm having my code return false on error, hence the `!`. --jpa
        return !USER_SOCK->handleFragment(data, length, remainingByteCount, opcode, fin);
    }


    // Explicitly generate code for template methods:

    template class WebSocketProtocol<SERVER>;
    template class WebSocketProtocol<CLIENT>;
}  // namespace uWS
