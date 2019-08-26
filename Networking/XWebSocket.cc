//
// XWebSocket.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "XWebSocket.hh"
#include "HTTPLogic.hh"
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
#include "StringUtil.hh"
#include "ThreadUtil.hh"
#include "sockpp/mbedtls_context.h"
#include "sockpp/exception.h"
#include <string>

using namespace litecore;
using namespace litecore::websocket;


void C4RegisterXWebSocket() {
    repl::C4SocketImpl::registerInternalFactory([](websocket::URL url,
                                                   websocket::Role role,
                                                   fleece::alloc_slice options) -> WebSocketImpl*
                                                {
        return new XWebSocket(url, role, fleece::AllocedDict(options));
    });
}


namespace litecore { namespace websocket {
    using namespace std;
    using namespace fleece;
    using namespace net;


    XWebSocket::XWebSocket(const URL &url,
                           Role role,
                           const fleece::AllocedDict &options)
    :WebSocketImpl(url, role, options, true)
    {
        slice pinnedCert = options[kC4ReplicatorOptionPinnedServerCert].asData();
        if (pinnedCert) {
            _tlsContext.reset(new sockpp::mbedtls_context);
            _tlsContext->allow_only_certificate(string(pinnedCert));
        }
        // TODO: Check kC4ReplicatorOptionAuthentication for client auth
    }


    XWebSocket::~XWebSocket() {
        logDebug("~XWebSocket");
        // This could be called from various threads, including the reader...
        if (_readerThread.joinable())
            _readerThread.detach();
    }


    void XWebSocket::connect() {
        // Spawn a thread to connect and run the read loop:
        WebSocketImpl::connect();
        retain(this);
        _readerThread = thread([&]() {_connect();});
    }


    void XWebSocket::closeSocket() {
        logVerbose("closeSocket");
        _socket->close();
        
        // Force reader & writer threads to wake up so they'll know the socket closed:
        sendBytes(alloc_slice());
        {
            unique_lock<mutex> lock(_receiveMutex);
            _receiveCond.notify_one();
        }
    }


    void XWebSocket::sendBytes(alloc_slice bytes) {
        _outbox.push(bytes);
    }


    void XWebSocket::receiveComplete(size_t byteCount) {
        unique_lock<mutex> lock(_receiveMutex);
        bool wasThrottled = (readCapacity() == 0);
        Assert(byteCount <= _receivedBytesPending);
        _receivedBytesPending -= byteCount;
        if (wasThrottled && readCapacity() > 0)
            _receiveCond.notify_one();
    }


    void XWebSocket::requestClose(int status, fleece::slice message) {
        Assert(false, "Should not be called");
    }


#pragma mark - BACKGROUND ACTIVITY:


    unique_ptr<XClientSocket> XWebSocket::_connectLoop() {
        Dict headers = options()[kC4ReplicatorOptionExtraHeaders].asDict();
        HTTPLogic logic {repl::Address(url()), Headers(headers)};
        logic.setWebSocketProtocol(options()[kC4SocketOptionWSProtocols].asString());
        bool usedAuth = false;
        while (true) {
            auto socket = make_unique<XClientSocket>(_tlsContext.get());
            switch (logic.sendNextRequest(*socket)) {
                case HTTPLogic::kSuccess:
                    gotHTTPResponse(int(logic.status()), logic.responseHeaders());
                    return socket;
                case HTTPLogic::kRetry:
                    break; // redirected; go around again
                case HTTPLogic::kAuthenticate: {
                    if (!usedAuth && !logic.authChallenge()->forProxy
                                  && logic.authChallenge()->type == "Basic") {
                        Dict auth = options()[kC4ReplicatorOptionAuthentication].asDict();
                        slice authType = auth[kC4ReplicatorAuthType].asString();
                        if (authType == slice(kC4AuthTypeBasic)) {
                            slice username = auth[kC4ReplicatorAuthUserName].asString();
                            slice password = auth[kC4ReplicatorAuthPassword].asString();
                            if (username && password) {
                                logic.setAuthHeader(HTTPLogic::basicAuth(username, password));
                                usedAuth = true;
                                break; // retry with credentials
                            }
                        }
                    }
                    // give up:
                    gotHTTPResponse(int(logic.status()), logic.responseHeaders());
                    closeWithError(error(error::WebSocket, int(logic.status())), "connect");
                    return nullptr;
                }
                case HTTPLogic::kFailure:
                    if (logic.status() != HTTPStatus::undefined)
                        gotHTTPResponse(int(logic.status()), logic.responseHeaders());
                    closeWithError(*logic.error(), "connect");
                    return nullptr;
            }
        }
    }


    // This runs on its own thread.
    void XWebSocket::_connect() {
        SetThreadName("WebSocket reader");
        try {
            // Connect:
            auto socket = _connectLoop();
            if (!socket) {
                release(this);
                return;
            }

            _socket = move(socket);
            onConnect();
        } catch (const std::exception &x) {
            closeWithError(x, "connect");
            release(this);
            return;
        }

        // OK, now we are connected -- start the loops for I/O:
        retain(this);
        _writerThread = thread([&]() {writeLoop();});
        readLoop();
    }


    // This runs on the same thread as _connect.
    void XWebSocket::readLoop() {
        try {
            while (true) {
                // Wait until there's room to read more data:
                size_t capacity;
                {
                    unique_lock<mutex> lock(_receiveMutex);
                    _receiveCond.wait(lock, [&]() {return readCapacity() > 0;});
                    capacity = readCapacity();
                }

                // Read from the socket:
                slice data = _socket->read(capacity);
                logDebug("Received %zu bytes from socket", data.size);
                if (data.size == 0)
                    break; // EOF

                // The bytes read count against the read-capacity:
                {
                    unique_lock<mutex> lock(_receiveMutex);
                    _receivedBytesPending += data.size;
                }

                // Dispatch to the client:
                onReceive(data);
            }
            logInfo("EOF on readLoop");
            onClose(0);

        } catch (const exception &x) {
            closeWithError(x, "readLoop");
        }
        _writerThread.join();
        release(this);
    }


    // This runs on its own thread.
    void XWebSocket::writeLoop() {
        SetThreadName("WebSocket writer");
        try {
            while (true) {
                alloc_slice data = _outbox.pop();
                if (!_socket->connected())
                    break;
                if (_socket->write_n(data) == 0)
                    break;
                logDebug("Wrote %zu bytes to socket", data.size);
                onWriteComplete(data.size);     // notify that data's been written
            }
            logInfo("EOF on writeLoop");
        } catch (const exception &x) {
            closeWithError(x, "writeLoop");
        }
        release(this);
    }


    void XWebSocket::closeWithError(const exception &x, const char *where) {
        // Convert exception to CloseStatus:
        error e = net::XSocket::convertException(x);
        logError("caught exception on %s: %s", where, e.what());
        closeWithError(e, where);
    }

    void XWebSocket::closeWithError(const error &e, const char *where) {
        alloc_slice message(e.what());
        CloseStatus status {kUnknownError, e.code, message};
        if (e.domain == error::WebSocket)
            status.reason = kWebSocketClose;
        else if (e.domain == error::POSIX)
            status.reason = kPOSIXError;
        else if (e.domain == error::Network)
            status.reason = kNetworkError;
        onClose(status);
    }

} }
