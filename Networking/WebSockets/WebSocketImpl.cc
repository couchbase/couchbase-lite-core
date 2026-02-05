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
#include "Error.hh"
#include "NumConversion.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include "WebSocketProtocol.hh"
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>

namespace litecore::websocket {
    using namespace std;
    using namespace uWS;
    using namespace fleece;

    using ClientProtocol = WebSocketProtocol<false>;
    using ServerProtocol = WebSocketProtocol<true>;
    using Timer          = actor::Timer;

    static constexpr size_t kSendBufferSize = 64 * 1024;

    // Default interval at which to send PING messages (configurable via options)
    static constexpr auto kDefaultHeartbeatInterval = chrono::seconds(5 * 60);

    // Timeout for disconnecting if no PONG response received
    static constexpr auto kPongTimeout = 10s;

    // Timeout for disconnecting if no CLOSE response received
    static constexpr auto kCloseTimeout = 5s;

    /** A subclass of Message that notifies WebSocketImpl when it's destructed, i.e. handled. */
    class MessageImpl : public Message {
      public:
        MessageImpl(WebSocketImpl* ws, alloc_slice data, bool binary)
            : Message(data, binary), _size(data.size), _webSocket(ws) {}

        ~MessageImpl() override { _webSocket->receiveComplete(_size); }

      private:
        size_t const         _size;
        WebSocketImpl* const _webSocket;
    };

    /** This is the actual implementation of WebSocketImpl.
     *
     * IMPORTANT: The thread-safety of this class is complicated!
     * - Methods called from outside (API calls or Timer callbacks) must acquire a unique_lock on
     *   `_mutex` while accessing mutable state.
     * - Subroutines called while the mutex is locked have "_"-prefixed names.
     *   They can freely access mutable state.
     * - A non-underscored method can only call an underscored method while locked,
     *   and it can only call another non-underscored method while _not_ locked.
     * - An underscored method cannot call a non-underscored method.
     * - Subclass methods or the delegate MUST NOT be called while holding the lock, because they
     *   might call back into WebSocketImpl and deadlock. That means they can only be called from
     *   non-underscored methods, and only when not locked.
     */
    struct WebSocketImpl::impl : LoggingProxy {
        class LockWithDefer;

        enum SocketLifecycleState : int { SOCKET_UNINIT, SOCKET_OPENING, SOCKET_OPENED, SOCKET_CLOSING, SOCKET_CLOSED };

        // Immutable state:
        Parameters const      _parameters;         // Client parameters
        WebSocketImpl&        _webSocket;          // The actual WebSocket object
        bool const            _framing;            // True if I parse WebSocket frames
        chrono::seconds const _heartbeatInterval;  // How often to send PINGs

        // Mutable state:
        mutex                      _mutex;                 // Protects access to everything below
        unique_ptr<ClientProtocol> _clientProtocol;        // 3rd party class that does the framing
        unique_ptr<ServerProtocol> _serverProtocol;        // 3rd party class that does the framing
        alloc_slice                _curMessage;            // Message being received
        uint8_t                    _curOpCode{};           // Opcode of msg in _curMessage
        size_t                     _curMessageLength{0};   // # of valid bytes in _curMessage
        size_t                     _bufferedBytes{0};      // # bytes written but not yet completed
        size_t                     _deliveredBytes{};      // Temporary count of bytes sent to delegate
        bool                       _closeSent{false};      // Close message sent?
        bool                       _closeReceived{false};  // Close message received?
        alloc_slice                _closeMessage;          // The encoded close request message
        Timer::time                _lastReceiveTime{};     // Time I last received a message
        unique_ptr<Timer>          _pingTimer;             // Fires when it's time to (maybe) send a PING
        unique_ptr<Timer>          _responseTimer;         // Fires when PONG times out
        bool                       _timerDisabled{false};  // If true, timeout is ignored
        chrono::seconds            _curTimeout{};          // Duration for _responseTimer
        bool                       _timedOut{false};       // True if _responseTimer timed out
        alloc_slice                _protocolError;         // Error message from WebSocketProtocol
        bool                       _didConnect{false};     // True if I've connected
        SocketLifecycleState       _socketLCState{};       // Lifecycle state
        LockWithDefer*             _lockWithDefer{};

        // Connection diagnostics, logged on close:
        Stopwatch _timeConnected{false};             // Time since socket opened
        uint64_t  _bytesSent{0}, _bytesReceived{0};  // Total byte count sent/received

        impl(WebSocketImpl& webSocket, bool framing, Parameters parameters)
            : LoggingProxy(&webSocket)
            , _parameters(std::move(parameters))
            , _webSocket{webSocket}
            , _framing(framing)
            , _heartbeatInterval{computeHeartbeatInterval(_framing, _parameters)}
            , _responseTimer(new actor::Timer([this] { timedOut(); })) {
            if ( framing ) {
                if ( webSocket.role() == Role::Server ) _serverProtocol = make_unique<ServerProtocol>();
                else
                    _clientProtocol = make_unique<ClientProtocol>();
            }
        }

        static chrono::seconds computeHeartbeatInterval(bool framing, Parameters const& parameters) {
            if ( !framing ) return 0s;
            else if ( parameters.heartbeatSecs > 0 )
                return chrono::seconds(parameters.heartbeatSecs);
            else
                return kDefaultHeartbeatInterval;
        }

        // Public API. Opens a connection.
        void connect() {
            unique_lock lock(_mutex);

            logInfo("Connecting...");
            _socketLCState = SOCKET_OPENING;
            _startResponseTimer(chrono::seconds(kConnectTimeoutSecs));
        }

        // Protected API. Subclass calls this when it's connected.
        void onConnect() {
            unique_lock lock(_mutex);

            if ( _socketLCState != SOCKET_OPENING ) {
                logInfo("WebSocket not in 'Opening' state, ignoring onConnect...");
                return;
            }

            logInfo("Connected!");
            _socketLCState = SOCKET_OPENED;
            _didConnect    = true;
            _responseTimer->stop();
            _timeConnected.start();
            _lastReceiveTime = Timer::clock::now();

            // Initialize ping timer.
            if ( _framing && _heartbeatInterval > 0s ) {
                logVerbose("Setting ping timer to %lld...", duration_cast<chrono::seconds>(_heartbeatInterval).count());
                _pingTimer = make_unique<actor::Timer>([this] { sendPing(); });
                (void)_schedulePing();
            }

            lock.unlock();  // UNLOCK to call delegate
            _webSocket.delegateWeak()->invoke(&Delegate::onWebSocketConnect);
        }

        // Public API. Sends a WebSocket message.
        bool send(slice message, bool binary) {
            logVerbose("Sending %zu-byte message", message.size);
            return sendOp(message, binary ? BINARY : TEXT);
        }

        bool sendOp(slice message, uint8_t opcode) {
            alloc_slice frame;
            bool        writeable;
            {
                unique_lock<mutex> lock(_mutex);

                if ( _closeSent && opcode != CLOSE ) {
                    warn("sendOp refusing to send msg type %d after close", opcode);
                    return false;
                }

                if ( _framing ) {
                    frame.resize(message.size + 10);  // maximum space needed
                    size_t newSize;
                    if ( _webSocket.role() == Role::Server ) {
                        newSize = ServerProtocol::formatMessage((byte*)frame.buf, (const char*)message.buf,
                                                                message.size, (OpCode)opcode, message.size, false);
                    } else {
                        newSize = ClientProtocol::formatMessage((byte*)frame.buf, (const char*)message.buf,
                                                                message.size, (OpCode)opcode, message.size, false);
                    }
                    frame.shorten(newSize);
                } else {
                    DebugAssert(opcode == BINARY);
                    frame = message;
                }
                _bufferedBytes += frame.size;
                writeable = (_bufferedBytes <= kSendBufferSize);
            }

            // Release the lock before calling sendBytes, because that's an abstract method, and some
            // implementation of it might call back into me and deadlock.
            _webSocket.sendBytes(frame);
            return writeable;
        }

        // Protected API. Called when an async write has completed.
        void onWriteComplete(size_t size) {
            LockWithDefer lock(this);

            _bytesSent += size;
            bool notify = (_bufferedBytes > kSendBufferSize);
            _bufferedBytes -= size;
            if ( _bufferedBytes > kSendBufferSize ) notify = false;

            if ( _closeSent && _closeReceived && _bufferedBytes == 0 ) {
                // My close message has gone through; now I can disconnect:
                logInfo("sent close echo; disconnecting socket now");
                _callCloseSocket();
            } else if ( notify ) {
                lock.unlock();  // UNLOCK to call delegate
                _webSocket.delegateWeak()->invoke(&Delegate::onWebSocketWriteable);
            }
        }

        // Protected API. Called when a WebSocket frame is received.
        void onReceive(slice data) {
            ssize_t completedBytes = 0;
            {
                // Lock the mutex; this protects all methods (below) involved in receiving,
                // since they're called from this one.
                LockWithDefer lock(this);

                if ( data.empty() && !_closeReceived ) {
                    // We assume empty data means a zero-length read, i.e. EOF
                    logError("Protocol error: Peer shutdown socket without a CLOSE message");
                    _gotProtocolError("Peer shutdown socket without a CLOSE message"_sl);
                    return;
                }

                _lastReceiveTime = Timer::clock::now();
                _bytesReceived += data.size;
                if ( _framing ) {
                    _deliveredBytes          = 0;
                    size_t prevMessageLength = _curMessageLength;
                    // this next line will call handleFragment(), below --
                    if ( _clientProtocol ) _clientProtocol->consume((byte*)data.buf, data.size, this);
                    else
                        _serverProtocol->consume((byte*)data.buf, data.size, this);
                    // Compute # of bytes consumed: just the framing data, not any partial or
                    // delivered messages. (Trust me, the math works.)
                    completedBytes =
                            narrow_cast<ssize_t>(data.size + prevMessageLength - _curMessageLength - _deliveredBytes);
                } else {
                    _deliverMessageToDelegate(alloc_slice(data));
                }
            }

            // After unlocking, tell subclass how many incoming bytes have been handled:
            if ( completedBytes > 0 ) _webSocket.receiveComplete(completedBytes);
        }

        // Called from inside _protocol->consume(), with the _mutex locked
        bool _handleFragment(byte* data, size_t length, size_t remainingBytes, uint8_t opCode, bool fin) {
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

            if ( length > 0 ) {
                memcpy((void*)&_curMessage[_curMessageLength], data, length);
                _curMessageLength += length;
            }

            // End:
            if ( fin && remainingBytes == 0 ) {
                _curMessage.shorten(_curMessageLength);
                bool ok = _receivedMessage(_curOpCode, std::move(_curMessage));
                DebugAssert(!_curMessage);
                _curMessageLength = 0;
                return ok;
            }
            return true;
        }

        bool _receivedMessage(uint8_t opCode, alloc_slice message) {
            switch ( opCode ) {
                case TEXT:
                    if ( !ClientProtocol::isValidUtf8((unsigned char*)message.buf, message.size) ) return false;
                    [[fallthrough]];
                case BINARY:
                    _deliverMessageToDelegate(std::move(message));
                    return true;
                case CLOSE:
                    return _receivedClose(message);
                case PING:
                    {
                        logInfo("Received PING -- sending PONG");
                        alloc_slice msgToSend = message ? message : alloc_slice(size_t(0));
                        defer([=, this] { sendOp(msgToSend, PONG); });
                        return true;
                    }
                case PONG:
                    _receivedPong();
                    return true;
                default:
                    return false;
            }
        }

        // Called from inside _protocol->consume(), with the _mutex locked
        void _gotProtocolError(slice message) {
            logError("Protocol error: %.*s", FMTSLICE(message));
            _protocolError = message;
            _callCloseSocket();
        }

        void _deliverMessageToDelegate(alloc_slice messageBody) {
            logVerbose("Received %zu-byte message", messageBody.size);
            _deliveredBytes += messageBody.size;
            auto message = make_retained<MessageImpl>(&_webSocket, std::move(messageBody), true);
            defer([=, this] { _webSocket.delegateWeak()->invoke(&Delegate::onWebSocketMessage, message); });
        }

#pragma mark - HEARTBEAT:

        // returns false, instead of scheduling, if a PING should be sent immediately.
        bool _schedulePing() {
            if ( _closeSent || _heartbeatInterval <= 0s ) return true;  // No PINGs
            Timer::duration delay = _lastReceiveTime + _heartbeatInterval - Timer::clock::now();
            if ( delay <= 0s ) return false;  // PING is needed immediately
            _pingTimer->fireAfter(delay);
            return true;
        }

        // timer callback
        void sendPing() {
            unique_lock lock(_mutex);

            if ( !_pingTimer ) {
                warn("Ping timer not available, giving up on sendPing...");
                return;
            }

            if ( _socketLCState == SOCKET_CLOSED ) {
                warn("Socket is already closed, giving up on sendPing...");
                return;
            }

            if ( _schedulePing() ) return;  // No PING is needed yet

            _startResponseTimer(min(kPongTimeout, _heartbeatInterval - 1s));
            // exit scope to release the lock -- this is needed before calling sendOp,
            // which acquires the lock itself

            logInfo("Sending PING");
            lock.unlock();  // UNLOCK to call sendOp()
            sendOp(nullslice, PING);
        }

        void _receivedPong() {
            logInfo("Received PONG");
            _responseTimer->stop();
            Assert(_schedulePing());
        }

        void _startResponseTimer(chrono::seconds timeoutSecs) {
            _curTimeout = timeoutSecs;
            _responseTimer->fireAfter(timeoutSecs);
        }

        // timer callback
        void timedOut() {
            LockWithDefer lock(this);

            if ( _timerDisabled ) return;
            if ( Timer::clock::now() - _lastReceiveTime < _curTimeout ) return;
            logError("No response received after %lld sec -- disconnecting", (long long)_curTimeout.count());
            _timedOut = true;

            switch ( _socketLCState ) {
                case SOCKET_OPENING:
                case SOCKET_OPENED:
                    if ( _framing ) _callCloseSocket();
                    else
                        _callRequestClose(504, "Timed out"_sl);
                    break;
                case SOCKET_CLOSING:
                    lock.unlock();  // UNLOCK to call onClose()
                    onClose({kNetworkError, kNetErrTimeout, nullslice});
                    break;
                default:
                    break;
            }
        }

#pragma mark - CLOSING:

        // See <https://tools.ietf.org/html/rfc6455#section-7>


        void _callCloseSocket() {
            if ( auto state = _socketLCState; state <= SOCKET_OPENED ) {
                if ( state != SOCKET_OPENED ) { logVerbose("Calling closeSocket before the socket is open"); }
                _socketLCState = SOCKET_CLOSING;
                _startResponseTimer(kCloseTimeout);
                defer([this] { _webSocket.closeSocket(); });
            } else {
                logVerbose("Calling closeSocket when the socket is %s",
                           state == SOCKET_CLOSING ? "pending close" : "already closed");
            }
        }

        void _callRequestClose(int status, slice message) {
            switch ( _socketLCState ) {
                case SOCKET_UNINIT:
                case SOCKET_OPENING:
                    logVerbose("Calling requestClose before the socket is connected");
                    [[fallthrough]];
                case SOCKET_OPENED:
                    {
                        _socketLCState = SOCKET_CLOSING;
                        _startResponseTimer(kCloseTimeout);
                        alloc_slice allocedMessage(message);
                        defer([=, this] { _webSocket.requestClose(status, allocedMessage); });
                        break;
                    }
                case SOCKET_CLOSING:
                    logVerbose("Calling requestClose when the socket is pending close");
                    break;
                case SOCKET_CLOSED:
                    logVerbose("Calling requestClose when the socket is already closed");
                    break;
            }
        }

        // Public API. Initiates a request to close the connection cleanly.
        void close(int status, slice message) {
            LockWithDefer lock(this);

            switch ( _socketLCState ) {
                case SOCKET_CLOSING:
                    logVerbose("Calling close when the socket is pending close");
                    break;
                case SOCKET_CLOSED:
                    logVerbose("Calling close when the socket is already closed");
                    break;
                case SOCKET_OPENED:
                    logInfo("Requesting close with status=%d, message='%.*s'", status, SPLAT(message));
                    if ( _framing ) {
                        if ( _closeSent || _closeReceived ) {
                            logVerbose("Close already processed (_closeSent: %d, _closeReceived: %d), exiting "
                                       "WebSocketImpl::close()",
                                       (int)_closeSent, (int)_closeReceived);
                            break;
                        }

                        auto closeMsg = alloc_slice(2 + message.size);
                        auto size     = ClientProtocol::formatClosePayload((byte*)closeMsg.buf, (uint16_t)status,
                                                                           (const char*)message.buf, message.size);
                        closeMsg.shorten(size);
                        _closeSent    = true;
                        _closeMessage = closeMsg;
                        _startResponseTimer(kCloseTimeout);
                        defer([=, this] { sendOp(closeMsg, CLOSE); });
                    } else {
                        _callRequestClose(status, message);
                    }
                    break;
                case SOCKET_OPENING:
                    logInfo("Closing socket before connection established...");
                    if ( _framing ) {
                        // The web socket is being requested to close before it's even connected, so just
                        // shortcut to the callback and make sure that onConnect does nothing now
                        _callCloseSocket();
                    } else {
                        _callRequestClose(status, message);
                    }
                    break;
                case SOCKET_UNINIT:
                    _callCloseSocket();
                    break;
            }
        }

        // Handles a close message received from the peer.
        bool _receivedClose(slice message) {
            if ( _closeReceived ) return false;
            _closeReceived = true;
            if ( _closeSent ) {
                // I initiated the close; the peer has confirmed, so disconnect the socket now:
                logInfo("Close confirmed by peer; disconnecting socket now");
                _callCloseSocket();
            } else {
                // Peer is initiating a close. Save its message and echo it:
                if ( willLog() ) {
                    auto close = ClientProtocol::parseClosePayload((byte*)message.buf, message.size);
                    logInfo("Client is requesting close (%d '%.*s'); echoing it", close.code, (int)close.length,
                            (char*)close.message);
                }
                _closeSent    = true;
                _closeMessage = message;
                defer([=, this] { sendOp(_closeMessage, CLOSE); });
            }
            _timerDisabled = true;
            return true;
        }

        // Protected API. Called when the peer requests closing the socket.
        void onCloseRequested(int status, slice message) {
            unique_lock lock(_mutex);
            DebugAssert(!_framing);
            _callRequestClose(status, message);
        }

        // Protected API. Called on a socket error.
        void onClose(int posixErrno) {
            alloc_slice message;
            if ( posixErrno ) message = slice(strerror(posixErrno));
            onClose({kPOSIXError, posixErrno, message});
        }

        // Protected API. Called when the underlying socket closes.
        void onClose(CloseStatus status) {
            unique_lock lock(_mutex);

            switch ( auto prevState = std::exchange(_socketLCState, SOCKET_CLOSED) ) {
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
                    warn("Unexpected _socketLCState %d", int(prevState));
                    return;
            }

            auto _logErrorForStatus = [this](const char* msg, const CloseStatus& cstatus) {
                if ( cstatus.message.empty() ) {
                    logError("%s (reason=%-s %d)", msg, cstatus.reasonName(), cstatus.code);
                } else {
                    logError("%s (reason=%-s %d) %.*s", msg, cstatus.reasonName(), cstatus.code,
                             SPLAT(cstatus.message));
                }
            };

            // CBL-6799. We try to avoid deleting the timer objects, _pingTimer and _responseTimer, because it's hard
            // to synchronize their uses and deletions. Instead, We disable them, which makes the callback function
            // a no-op function. The timers will be deleted with "this" object.
            _timerDisabled = true;

            if ( status.reason == kWebSocketClose ) {
                if ( _timedOut ) status = {kNetworkError, kNetErrTimeout, nullslice};
                else if ( _protocolError ) {
                    status = {kWebSocketClose, kCodeProtocolError, _protocolError};
                    _logErrorForStatus("WebSocketImpl::onClose", status);
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
                        stringstream ss;
                        ss << "Unexpected or unclean socket disconnect!";
                        if ( !_closeSent ) { ss << " (close not sent"; }
                        if ( !_closeReceived ) {
                            ss << (_closeSent ? " (" : "; ");
                            ss << "close not received)";
                        } else if ( !_closeSent ) {
                            ss << ")";
                        }
                        _logErrorForStatus(std::move(ss).str().c_str(), status);
                    }

                    if ( clean ) {
                        status.reason = kWebSocketClose;
                        if ( !expected ) status.code = kCodeAbnormal;
                        else if ( !_closeMessage )
                            status.code = kCodeNormal;
                        else {
                            auto msg = ClientProtocol::parseClosePayload((byte*)_closeMessage.buf, _closeMessage.size);
                            status.code    = msg.code ? msg.code : kCodeStatusCodeExpected;
                            status.message = slice(msg.message, msg.length);
                        }
                    }
                    _closeMessage = nullslice;
                } else {
                    if ( clean ) logInfo("WebSocket closed normally");
                    else
                        _logErrorForStatus("WebSocket closed abnormally", status);
                }

                _timeConnected.stop();
                double t = _timeConnected.elapsed();
                logInfo("sent %" PRIu64 " bytes, rcvd %" PRIu64 ", in %.3f sec (%.0f/sec, %.0f/sec)", _bytesSent,
                        _bytesReceived, t, double(_bytesSent) / t, double(_bytesReceived) / t);
            } else {
                _logErrorForStatus("WebSocket failed to connect!", status);
            }

            if ( auto delegate = _webSocket.delegateWeak() ) {
                lock.unlock();  // UNLOCK to call delegate
                delegate->invoke(&Delegate::onWebSocketClose, status);
            }
        }

        /** Utility class that locks `_mutex` and enables use of the `defer()` function below.
         *  It should be instantiated as `LockWithDefer lock(this);`. */
        class LockWithDefer {
          public:
            explicit LockWithDefer(impl* owner) : _owner(owner), _lock(owner->_mutex) {
                DebugAssert(owner->_lockWithDefer == nullptr);
                owner->_lockWithDefer = this;
            }

            void defer(function<void()> action) {
                DebugAssert(_lock.owns_lock());
                _actions.emplace_back(std::move(action));
            }

            void unlock() {
                DebugAssert(_lock.owns_lock());
                _owner->_lockWithDefer = nullptr;
                _lock.unlock();
            }

            ~LockWithDefer() {
                if ( _lock.owns_lock() ) _owner->_lockWithDefer = nullptr;
                if ( !_actions.empty() ) {
                    _lock.unlock();
                    for ( auto& action : _actions ) {
                        try {
                            action();
                        } catch ( ... ) {
#ifdef _MSC_VER
                            C4Error::warnCurrentException(__FUNCSIG__);
#else
                            C4Error::warnCurrentException(__PRETTY_FUNCTION__);
#endif
                        }
                    }
                }
            }

          private:
            impl* const              _owner;
            unique_lock<mutex>       _lock;
            vector<function<void()>> _actions;  // can't use smallVector: std::function is not trivially moveable
        };

        /// Schedules a function to be called immediately after the current lock is released.
        /// Precondition: Some caller must have a local `LockWithDefer` instance.
        void defer(function<void()> fn) { _lockWithDefer->defer(std::move(fn)); }
    };

#pragma mark - WEBSOCKET IMPL:

    WebSocketImpl::WebSocketImpl(const URL& url, Role role, bool framing, Parameters parameters)
        : WebSocket(url, role), Logging(WSLogDomain), _impl{make_unique<impl>(*this, framing, std::move(parameters))} {}

    const WebSocketImpl::Parameters& WebSocketImpl::parameters() const { return _impl->_parameters; }

    const AllocedDict& WebSocketImpl::options() const { return _impl->_parameters.options; }

    WebSocketImpl::~WebSocketImpl() = default;

    string WebSocketImpl::loggingIdentifier() const { return string(url()); }

    void WebSocketImpl::connect() { _impl->connect(); }

    bool WebSocketImpl::send(slice message, bool binary) { return _impl->send(message, binary); }

    void WebSocketImpl::close(int status, slice message) { _impl->close(status, message); }

    void WebSocketImpl::onConnect() { _impl->onConnect(); }

    void WebSocketImpl::onCloseRequested(int status, slice message) { _impl->onCloseRequested(status, message); }

    void WebSocketImpl::onClose(int posixErrno) { _impl->onClose(posixErrno); }

    void WebSocketImpl::onClose(CloseStatus status) { _impl->onClose(std::move(status)); }

    void WebSocketImpl::onReceive(slice data) { _impl->onReceive(data); }

    void WebSocketImpl::onWriteComplete(size_t byteCount) { _impl->onWriteComplete(byteCount); }

}  // namespace litecore::websocket

#pragma mark - WEBSOCKETPROTOCOL

// The rest of the implementation of WebSocketProtocol, which calls into WebSocket:
namespace uWS {

    static constexpr size_t kMaxMessageLength = 1 << 20;


// The `user` parameter points to the owning WebSocketImpl object.
#define USER_SOCK (static_cast<litecore::websocket::WebSocketImpl::impl*>(user))

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::setCompressed(void* /*user*/) {
        return false;  //TODO: Implement compression
    }

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::refusePayloadLength(void* /*user*/, size_t length) {
        return length > kMaxMessageLength;
    }

    template <const bool isServer>
    void WebSocketProtocol<isServer>::forceClose(void* user, const char* reason) {
        std::stringstream ss;
        ss << "WebSocketProtocol<" << (isServer ? "server" : "client") << ">::forceClose";
        if ( reason != nullptr ) { ss << reason; }
        USER_SOCK->_gotProtocolError(ss.str());
    }

    template <const bool isServer>
    bool WebSocketProtocol<isServer>::handleFragment(std::byte* data, size_t length, size_t remainingByteCount,
                                                     uint8_t opcode, bool fin, void* user) {
        // WebSocketProtocol expects this method to return true on error, but this confuses me
        // so I'm having my code return false on error, hence the `!`. --jpa
        return !USER_SOCK->_handleFragment(data, length, remainingByteCount, opcode, fin);
    }


    // Explicitly generate code for template methods:

    template class WebSocketProtocol<SERVER>;
    template class WebSocketProtocol<CLIENT>;
}  // namespace uWS
