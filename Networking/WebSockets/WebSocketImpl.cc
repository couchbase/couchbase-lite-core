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
#include <chrono>
#include <functional>
#include <string>

using namespace std;
using namespace uWS;
using namespace fleece;

namespace litecore { namespace websocket {

    static constexpr size_t kSendBufferSize = 64 * 1024;

    // Timeout for WebSocket connection (until HTTP response received)
    constexpr long WebSocketImpl::kConnectTimeoutSecs;

    // Default interval at which to send PING messages (configurable via options)
    static constexpr auto kDefaultHeartbeatInterval = chrono::seconds(5 * 60);

    // Timeout for disconnecting if no PONG response received
    static constexpr auto kPongTimeout  = 10s;

    // Timeout for disconnecting if no CLOSE response received
    static constexpr auto kCloseTimeout =  5s;

    
    class MessageImpl : public Message {
    public:
        MessageImpl(WebSocketImpl *ws, slice data, bool binary)
        :Message(data, binary)
        ,_size(data.size)
        ,_webSocket(ws)
        { }

        ~MessageImpl() {
            _webSocket->receiveComplete(_size);
        }

    private:
        size_t const _size;
        WebSocketImpl* const _webSocket;
    };



    WebSocketImpl::WebSocketImpl(const URL &url,
                                 Role role,
                                 bool framing,
                                 const Parameters &parameters)
    :WebSocket(url, role)
    ,Logging(WSLogDomain)
    ,_parameters(parameters)
    ,_framing(framing)
    ,_responseTimer(new actor::Timer(bind(&WebSocketImpl::timedOut, this)))
    {
        if (framing) {
            if (role == Role::Server)
                _serverProtocol.reset(new ServerProtocol);
            else
                _clientProtocol.reset(new ClientProtocol);
        }
    }

    WebSocketImpl::~WebSocketImpl() =default;


    string WebSocketImpl::loggingIdentifier() const {
        return string(url());
    }


    void WebSocketImpl::connect() {
        logInfo("Connecting...");
        startResponseTimer(chrono::seconds(kConnectTimeoutSecs));
    }


    void WebSocketImpl::gotHTTPResponse(int status, const websocket::Headers &headersFleece) {
        logInfo("Got HTTP response (status %d)", status);
        delegateWeak()->invoke(&Delegate::onWebSocketGotHTTPResponse, status, headersFleece);
    }

    void WebSocketImpl::onConnect() {
        if(_closed) {
            // If the WebSocket has already been closed, which only happens in rare cases
            // such as stopping a Replicator during the connecting phase, then don't continue...
            warn("WebSocket already closed, ignoring onConnect...");
            return;
        }
        
        logInfo("Connected!");
        _didConnect = true;
        _responseTimer->stop();
        _timeConnected.start();
        delegateWeak()->invoke(&Delegate::onWebSocketConnect);

        // Initialize ping timer. (This is the first time it's accessed, and this method is only
        // called once, so no locking is needed.)
        if (_framing) {
            if (heartbeatInterval() > 0) {
                _pingTimer.reset(new actor::Timer(bind(&WebSocketImpl::sendPing, this)));
                schedulePing();
            }
        }
    }


    bool WebSocketImpl::send(fleece::slice message, bool binary) {
        logVerbose("Sending %zu-byte message", message.size);
        return sendOp(message, binary ? uWS::BINARY : uWS::TEXT);
    }


    bool WebSocketImpl::sendOp(fleece::slice message, int opcode) {
        alloc_slice frame;
        bool writeable;
        {
            lock_guard<std::mutex> lock(_mutex);
            if (_closeSent && opcode != CLOSE)
                return false;
            if (_framing) {
                frame.resize(message.size + 10); // maximum space needed
                size_t newSize;
                if (role() == Role::Server) {
                    newSize = ServerProtocol::formatMessage((char*)frame.buf,
                                                            (const char*)message.buf, message.size,
                                                            (uWS::OpCode)opcode, message.size,
                                                            false);
                } else {
                    newSize = ClientProtocol::formatMessage((char*)frame.buf,
                                                            (const char*)message.buf, message.size,
                                                            (uWS::OpCode)opcode, message.size,
                                                            false);
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
            if (_bufferedBytes > kSendBufferSize)
                notify = false;

            disconnect = (_closeSent && _closeReceived && _bufferedBytes == 0);
        }

        if (disconnect) {
            // My close message has gone through; now I can disconnect:
            logInfo("sent close echo; disconnecting socket now");
            closeSocket();
        } else if (notify) {
            delegateWeak()->invoke(&Delegate::onWebSocketWriteable);
        }
    }


    void WebSocketImpl::onReceive(slice data) {
        ssize_t completedBytes = 0;
        int opToSend = 0;
        alloc_slice msgToSend;
        {
            // Lock the mutex; this protects all methods (below) involved in receiving,
            // since they're called from this one.
            lock_guard<mutex> lock(_mutex);

            if (data.empty() && !_closeReceived) {
                // We assume empty data means a zero-length read, i.e. EOF
                logError("Protocol error: Peer shutdown socket without a CLOSE message");
                protocolError();
                return;
            }

            _bytesReceived += data.size;
            if (_framing) {
                _deliveredBytes = 0;
                size_t prevMessageLength = _curMessageLength;
                // this next line will call handleFragment(), below --
                if (_clientProtocol)
                    _clientProtocol->consume((const char*)data.buf, (unsigned)data.size, this);
                else
                    _serverProtocol->consume((const char*)data.buf, (unsigned)data.size, this);
                opToSend = _opToSend;
                msgToSend = move(_msgToSend);
                // Compute # of bytes consumed: just the framing data, not any partial or
                // delivered messages. (Trust me, the math works.)
                completedBytes = data.size + prevMessageLength - _curMessageLength - _deliveredBytes;
            }
        }
        if (!_framing)
            deliverMessageToDelegate(data, true);

        if (completedBytes > 0)
            receiveComplete(completedBytes);

        // Send any message that was generated during the locked block above:
        if (msgToSend)
            sendOp(msgToSend, opToSend);
    }


    // Called from inside _protocol->consume(), with the _mutex locked
    bool WebSocketImpl::handleFragment(char *data,
                                       size_t length,
                                       unsigned int remainingBytes,
                                       int opCode,
                                       bool fin)
    {
        // Beginning:
        if (!_curMessage) {
            _curOpCode = opCode;
            _curMessage.reset(length + remainingBytes);
            _curMessageLength = 0;
        }

        // Body:
        if (_curMessageLength + length > _curMessage.size)
            return false; // overflow!

        // CBL-2169: addressing the 0-th element of 0-length slice will trigger assertion failure.
        if (length > 0) {
            memcpy((void*)&_curMessage[_curMessageLength], data, length);
            _curMessageLength += length;
        }

        // End:
        if (fin && remainingBytes == 0) {
            _curMessage.shorten(_curMessageLength);
            bool ok = receivedMessage(_curOpCode, move(_curMessage));
            DebugAssert(!_curMessage);
            _curMessageLength = 0;
            return ok;
        }
        return true;
    }


    // Called from handleFragment, with the mutex locked
    bool WebSocketImpl::receivedMessage(int opCode, alloc_slice message) {
        switch (opCode) {
            case TEXT:
                if (!ClientProtocol::isValidUtf8((unsigned char*)message.buf, message.size))
                    return false;
                // fall through:
            case BINARY:
                deliverMessageToDelegate(message, (opCode==BINARY));
                return true;
            case CLOSE:
                return receivedClose(message);
            case PING:
                _opToSend = PONG;
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
    void WebSocketImpl::protocolError() {
        _protocolError = true;
        closeSocket();
    }


    void WebSocketImpl::deliverMessageToDelegate(slice data, bool binary) {
        logVerbose("Received %zu-byte message", data.size);
        _deliveredBytes += data.size;
        Retained<Message> message(new MessageImpl(this, data, true));
        delegateWeak()->invoke(&Delegate::onWebSocketMessage, message);
    }


#pragma mark - HEARTBEAT:


    int WebSocketImpl::heartbeatInterval() const {
        if (!_framing)
            return 0;
        else if (_parameters.heartbeatSecs > 0)
            return _parameters.heartbeatSecs;
        else
            return (int)kDefaultHeartbeatInterval.count();
    }


    void WebSocketImpl::schedulePing() {
        if (!_closeSent)
            _pingTimer->fireAfter(chrono::seconds(heartbeatInterval()));
    }


    // timer callback
    void WebSocketImpl::sendPing() {
        {
            lock_guard<mutex> lock(_mutex);
            if (!_pingTimer)
                return;
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
        if (_responseTimer)
            _responseTimer->fireAfter(timeoutSecs);
    }


    void WebSocketImpl::timedOut() {
        logError("No response received after %lld sec -- disconnecting",
                 (long long)_curTimeout.count());
        _timedOut = true;
        if (_framing)
            closeSocket();
        else
            requestClose(504, "Timed out"_sl);
    }


#pragma mark - CLOSING:


    // See <https://tools.ietf.org/html/rfc6455#section-7>


    // Initiates a request to close the connection cleanly.
    void WebSocketImpl::close(int status, fleece::slice message) {
        if(!_didConnect && _framing) {
            // The web socket is being requested to close before it's even connected, so just
            // shortcut to the callback and make sure that onConnect does nothing now
            closeSocket();
            _closed = true;
            
            // CBL-1088: If this is not called here, it never will be since the above _closed = true
            // prevents it from happening later.  This means that the Replicator using this connection
            // will never be informed of the connection close and will never reach the stopped state
            websocket::CloseStatus ss {kWebSocketClose, status, message};
            delegateWeak()->invoke(&Delegate::onWebSocketClose, ss);
            return;
        }
        
        logInfo("Requesting close with status=%d, message='%.*s'", status, SPLAT(message));
        if (_framing) {
            alloc_slice closeMsg;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_closeSent || _closeReceived)
                    return;
                closeMsg = alloc_slice(2 + message.size);
                auto size = ClientProtocol::formatClosePayload((char*)closeMsg.buf,
                                                               (uint16_t)status,
                                                               (char*)message.buf, message.size);
                closeMsg.shorten(size);
                _closeSent = true;
                _closeMessage = closeMsg;
                startResponseTimer(kCloseTimeout);
            }
            sendOp(closeMsg, uWS::CLOSE);
        } else {
            requestClose(status, message);
        }
    }


    // Handles a close message received from the peer. (Mutex is locked!)
    bool WebSocketImpl::receivedClose(slice message) {
        if (_closeReceived)
            return false;
        _closeReceived = true;
        if (_closeSent) {
            // I initiated the close; the peer has confirmed, so disconnect the socket now:
            logInfo("Close confirmed by peer; disconnecting socket now");
            closeSocket();
        } else {
            // Peer is initiating a close. Save its message and echo it:
            if (willLog()) {
                auto close = ClientProtocol::parseClosePayload((char*)message.buf, message.size);
                logInfo("Client is requesting close (%d '%.*s'); echoing it",
                    close.code, (int)close.length, close.message);
            }
            _closeSent = true;
            _closeMessage = message;
            // Don't send the message now or I'll deadlock; remember to do it later in onReceive:
            _msgToSend = message;
            _opToSend = CLOSE;
        }
        _pingTimer.reset();
        _responseTimer.reset();
        return true;
    }


    void WebSocketImpl::onCloseRequested(int status, fleece::slice message) {
        DebugAssert(!_framing);
        requestClose(status, message);
    }


    void WebSocketImpl::onClose(int posixErrno) {
        alloc_slice message;
        if (posixErrno)
            message = slice(strerror(posixErrno));
        onClose({kPOSIXError, posixErrno, message});
    }


    // Called when the underlying socket closes.
    void WebSocketImpl::onClose(CloseStatus status) {
        {
            lock_guard<mutex> lock(_mutex);

            if (_closed)
                return; // Guard against multiple calls to onClose

            _pingTimer.reset();

            if(!_timedOut) {
                // CBL-2410: If _timedOut is true then we are almost
                // certainly in this method synchronously from the _responseTimer
                // callback which means resetting here would cause a hang.  Since
                // _timedOut is true, this timer has already fired anyway so there
                // is no pressing need for a tear down here, it can wait until later.
                _responseTimer.reset();
            }

            if (status.reason == kWebSocketClose) {
                if (_timedOut)
                    status = {kNetworkError, kNetErrTimeout, nullslice};
                else if (_protocolError)
                    status = {kWebSocketClose, kCodeProtocolError, nullslice};
            }
            
            if (_didConnect) {
                bool clean = status.code == 0 ||
                            (status.reason == kWebSocketClose &&
                                    (status.code == kCodeNormal || status.code == kCodeGoingAway));
                if (_framing) {
                    bool expected = (_closeSent && _closeReceived);
                    if (expected && clean)
                        logInfo("Socket disconnected cleanly");
                    else
                        logError("Unexpected or unclean socket disconnect! (reason=%-s %d)",
                                 status.reasonName(), status.code);

                    if (clean) {
                        status.reason = kWebSocketClose;
                        if (!expected)
                            status.code = kCodeAbnormal;
                        else if (!_closeMessage)
                            status.code = kCodeNormal;
                        else {
                            auto msg = ClientProtocol::parseClosePayload((char*)_closeMessage.buf,
                                                                         _closeMessage.size);
                            status.code = msg.code ? msg.code : kCodeStatusCodeExpected;
                            status.message = slice(msg.message, msg.length);
                        }
                    }
                    _closeMessage = nullslice;
                } else {
                    if (clean)
                        logInfo("WebSocket closed normally");
                    else
                        logError("WebSocket closed abnormally (reason=%-s %d)",
                                 status.reasonName(), status.code);
                }

                _timeConnected.stop();
                double t = _timeConnected.elapsed();
                logInfo("sent %" PRIu64 " bytes, rcvd %" PRIu64 ", in %.3f sec (%.0f/sec, %.0f/sec)",
                    _bytesSent, _bytesReceived, t,
                    _bytesSent/t, _bytesReceived/t);
            } else {
                logError("WebSocket failed to connect! (reason=%-s %d)",
                         status.reasonName(), status.code);
            }

            _closed = true;
        }
        delegateWeak()->invoke(&Delegate::onWebSocketClose, status);
    }

} }


#pragma mark - WEBSOCKETPROTOCOL


// The rest of the implementation of uWS::WebSocketProtocol, which calls into WebSocket:
namespace uWS {

    static constexpr size_t kMaxMessageLength = 1<<20;


    // The `user` parameter points to the owning WebSocketImpl object.
    #define _sock ((litecore::websocket::WebSocketImpl*)user)


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::setCompressed(void *user) {
        return false;   //TODO: Implement compression
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::refusePayloadLength(void *user, int length) {
        return length > kMaxMessageLength;
    }


    template <const bool isServer>
    void WebSocketProtocol<isServer>::forceClose(void *user) {
        _sock->protocolError();
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::handleFragment(char *data,
                                                     size_t length,
                                                     unsigned int remainingByteCount,
                                                     int opcode,
                                                     bool fin,
                                                     void *user)
    {
        // WebSocketProtocol expects this method to return true on error, but this confuses me
        // so I'm having my code return false on error, hence the `!`. --jpa
        return ! _sock->handleFragment(data, length, remainingByteCount, opcode, fin);
    }


    // Explicitly generate code for template methods:

    template class WebSocketProtocol<SERVER>;
    template class WebSocketProtocol<CLIENT>;
}
