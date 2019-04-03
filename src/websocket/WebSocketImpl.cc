//
// WebSocketImpl.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
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

    static constexpr int kDefaultHeartbeatInterval = 5 * 60;

    
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
                                 const fleece::AllocedDict &options,
                                 bool framing)
    :WebSocket(url, role)
    ,Logging(WSLogDomain)
    ,_options(options)
    ,_framing(framing)
    {
        if (framing) {
            if (role == Role::Server)
                _serverProtocol.reset(new ServerProtocol);
            else
                _clientProtocol.reset(new ClientProtocol);
        }
    }

    WebSocketImpl::~WebSocketImpl()
    { }


    string WebSocketImpl::loggingIdentifier() const {
        return string(url());
    }


    void WebSocketImpl::gotHTTPResponse(int status, const fleece::AllocedDict &headersFleece) {
        delegate().onWebSocketGotHTTPResponse(status, headersFleece);
    }

    void WebSocketImpl::onConnect() {
        _timeConnected.start();
        delegate().onWebSocketConnect();

        // Initialize ping timer. (This is the first time it's accessed, and this method is only
        // called once, so no locking is needed.)
        if (heartbeatInterval() > 0) {
            _pingTimer.reset(new actor::Timer(bind(&WebSocketImpl::sendPing, this)));
            schedulePing();
        }
    }


    bool WebSocketImpl::send(fleece::slice message, bool binary) {
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
            delegate().onWebSocketWriteable();
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
                completedBytes = data.size - _deliveredBytes -
                                                (_curMessageLength - prevMessageLength);
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
        memcpy((void*)&_curMessage[_curMessageLength], data, length);
        _curMessageLength += length;

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


    void WebSocketImpl::deliverMessageToDelegate(slice data, bool binary) {
        _deliveredBytes += data.size;
        Retained<Message> message(new MessageImpl(this, data, true));
        delegate().onWebSocketMessage(message);
    }


#pragma mark - HEARTBEAT:


    int WebSocketImpl::heartbeatInterval() const {
        if (!_framing)
            return 0;
        fleece::Value heartbeat = options().get(kHeartbeatOption);
        if (heartbeat.type() == kFLNumber)
            return (int)heartbeat.asInt();
        else
            return kDefaultHeartbeatInterval;
    }


    void WebSocketImpl::schedulePing() {
        _pingTimer->fireAfter(chrono::seconds(heartbeatInterval()));
    }


    // timer callback
    void WebSocketImpl::sendPing() {
        {
            lock_guard<mutex> lock(_mutex);
            if (!_pingTimer)
                return;
            logInfo("Sending PING");
            schedulePing();
            // exit scope to release the lock -- this is needed before calling sendOp,
            // which acquires the lock itself
        }
        sendOp(nullslice, PING);
    }


    void WebSocketImpl::receivedPong() {
        logInfo("Received PONG");
    }


#pragma mark - CLOSING:


    // See <https://tools.ietf.org/html/rfc6455#section-7>


    // Initiates a request to close the connection cleanly.
    void WebSocketImpl::close(int status, fleece::slice message) {
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

            _pingTimer.reset();
            if (_framing) {
                bool clean = (status.code == 0
                              || (status.reason == kWebSocketClose && status.code == kCodeNormal));
                bool expected = (_closeSent && _closeReceived);
                if (expected && clean)
                    logInfo("Socket disconnected cleanly");
                else
                    warn("Unexpected or unclean socket disconnect! (reason=%-s, code=%d)",
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
                if (status.reason == kWebSocketClose) {
                    if (status.code != kCodeNormal && status.code != kCodeGoingAway)
                        warn("WebSocket closed abnormally with status %d", status.code);
                } else if (status.code != 0) {
                    logInfo("Socket disconnected! (reason=%d, code=%d)", status.reason, status.code);
                }
            }

            _timeConnected.stop();
            double t = _timeConnected.elapsed();
            logInfo("sent %" PRIu64 " bytes, rcvd %" PRIu64 ", in %.3f sec (%.0f/sec, %.0f/sec)",
                _bytesSent, _bytesReceived, t,
                _bytesSent/t, _bytesReceived/t);
        }

        delegate().onWebSocketClose(status);
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
        _sock->closeSocket();
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
