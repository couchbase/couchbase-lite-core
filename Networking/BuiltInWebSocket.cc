//
// BuiltInWebSocket.cc
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

#include "BuiltInWebSocket.hh"
#include "HTTPLogic.hh"
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
#include "c4.hh"
#include "StringUtil.hh"
#include "ThreadUtil.hh"
#include "sockpp/mbedtls_context.h"
#include "sockpp/exception.h"
#include <string>

using namespace litecore;
using namespace litecore::websocket;


void C4RegisterBuiltInWebSocket() {
    repl::C4SocketImpl::registerInternalFactory([](websocket::URL url,
                                                   websocket::Role role,
                                                   fleece::alloc_slice options) -> WebSocketImpl*
                                                {
        return new BuiltInWebSocket(url, role, fleece::AllocedDict(options));
    });
}


namespace litecore { namespace websocket {
    using namespace std;
    using namespace fleece;
    using namespace net;


    static constexpr size_t kReadBufferSize = 8192;


    BuiltInWebSocket::BuiltInWebSocket(const URL &url,
                                       Role role,
                                       const fleece::AllocedDict &options)
    :WebSocketImpl(url, role, options, true)
    {
        slice pinnedCert = options[kC4ReplicatorOptionPinnedServerCert].asData();
        if (pinnedCert) {
            _tlsContext.reset(new sockpp::mbedtls_context);
            _tlsContext->allow_only_certificate(string(pinnedCert));
        }
    }


    BuiltInWebSocket::~BuiltInWebSocket() {
        logDebug("~BuiltInWebSocket");
        // This could be called from various threads, including the reader...
        if (_readerThread.joinable())
            _readerThread.detach();
    }


    void BuiltInWebSocket::connect() {
        // Spawn a thread to connect and run the read loop:
        WebSocketImpl::connect();
        retain(this);
        _readerThread = thread([&]() {_connect();});
    }


    void BuiltInWebSocket::closeSocket() {
        logVerbose("closeSocket");
        _socket->close();
        
        // Force reader & writer threads to wake up so they'll know the socket closed:
        sendBytes(alloc_slice());
        {
            unique_lock<mutex> lock(_receiveMutex);
            _receiveCond.notify_one();
        }
    }


    void BuiltInWebSocket::sendBytes(alloc_slice bytes) {
        _outbox.push(bytes);
    }


    void BuiltInWebSocket::receiveComplete(size_t byteCount) {
        unique_lock<mutex> lock(_receiveMutex);
        bool wasThrottled = (readCapacity() == 0);
        Assert(byteCount <= _receivedBytesPending);
        _receivedBytesPending -= byteCount;
        if (wasThrottled && readCapacity() > 0)
            _receiveCond.notify_one();
    }


    void BuiltInWebSocket::requestClose(int status, fleece::slice message) {
        Assert(false, "Should not be called");
    }


#pragma mark - BACKGROUND ACTIVITY:


    // This runs on its own thread.
    void BuiltInWebSocket::_connect() {
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
            closeWithException(x, "connect");
            release(this);
            return;
        }

        // OK, now we are connected -- start the loops for I/O:
        retain(this);
        _writerThread = thread([&]() {writeLoop();});
        readLoop();
    }


    bool BuiltInWebSocket::configureProxy(HTTPLogic &logic, Dict proxyOpt) {
        if (!proxyOpt)
            return true;
        slice typeStr = proxyOpt[kC4ReplicatorProxyType].asString();
        if (typeStr == nullslice || typeStr == slice(kC4ProxyTypeNone)) {
            logic.setProxy({});
        } else {
            ProxyType type;
            if (typeStr == slice(kC4ProxyTypeHTTP))
                type = ProxyType::HTTP;
            else if (typeStr == slice(kC4ProxyTypeCONNECT))
                type = ProxyType::CONNECT;
            else
                return false;
            Dict auth = proxyOpt[kC4ReplicatorProxyAuth].asDict();
            if (auth)
                return false; // TODO: Proxy auth
            try {
                ProxySpec proxy {type, Address(proxyOpt[kC4ReplicatorProxyURL].asString())};
                logic.setProxy(proxy);
            } catch (...) {return false;}   // Address constructor throws on invalid URL
        }
        return true;
    }


    unique_ptr<ClientSocket> BuiltInWebSocket::_connectLoop() {
        Dict headers = options()[kC4ReplicatorOptionExtraHeaders].asDict();
        HTTPLogic logic {Address(url()), Headers(headers)};
        logic.setWebSocketProtocol(options()[kC4SocketOptionWSProtocols].asString());

        if (!configureProxy(logic, options()[kC4ReplicatorOptionProxyServer].asDict())) {
            closeWithError(c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter,
                                        "Invalid/unsupported proxy settings"_sl),
                           "connect");
            return nullptr;
        }

        bool usedAuth = false;
        unique_ptr<ClientSocket> socket;
        HTTPLogic::Disposition lastDisposition = HTTPLogic::kFailure;
        while (true) {
            if (lastDisposition != HTTPLogic::kContinue)
                socket = make_unique<ClientSocket>(_tlsContext.get());
            switch (logic.sendNextRequest(*socket)) {
                case HTTPLogic::kSuccess:
                    gotHTTPResponse(int(logic.status()), logic.responseHeaders());
                    return socket;
                case HTTPLogic::kRetry:
                    break; // redirected; go around again
                case HTTPLogic::kContinue:
                    break; // Will continue with the same socket (after connecting to a proxy)
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
                    closeWithError(c4error_make(WebSocketDomain, int(logic.status()), nullslice),
                                   "connect");
                    return nullptr;
                }
                case HTTPLogic::kFailure:
                    if (logic.status() != HTTPStatus::undefined)
                        gotHTTPResponse(int(logic.status()), logic.responseHeaders());
                    closeWithError(logic.error(), "connect");
                    return nullptr;
            }
        }
    }


    // This runs on the same thread as _connect.
    void BuiltInWebSocket::readLoop() {
        try {
            alloc_slice buffer(kReadBufferSize);
            while (true) {
                // Wait until there's room to read more data:
                size_t capacity;
                {
                    unique_lock<mutex> lock(_receiveMutex);
                    _receiveCond.wait(lock, [&]() {return readCapacity() > 0;});
                    capacity = readCapacity();
                }

                // Read from the socket:
                ssize_t n = _socket->read((void*)buffer.buf, min(buffer.size, capacity));
                logDebug("Received %zu bytes from socket", n);
                if (_usuallyFalse(n <= 0))
                    break;

                // The bytes read count against the read-capacity:
                {
                    unique_lock<mutex> lock(_receiveMutex);
                    _receivedBytesPending += n;
                }

                // Pass data to WebSocket parser:
                onReceive(slice(buffer.buf, n));
            }

            logInfo("End of readLoop; err = %s", c4error_descriptionStr(_socket->error()));
            closeWithError(_socket->error(), "readLoop");

        } catch (const exception &x) {
            closeWithException(x, "readLoop");
        }
        _writerThread.join();
        release(this);
    }


    // This runs on its own thread.
    void BuiltInWebSocket::writeLoop() {
        SetThreadName("WebSocket writer");
        try {
            while (true) {
                alloc_slice data = _outbox.pop();
                if (!_socket->connected())
                    break;
                if (_socket->write_n(data) <= 0)
                    break;
                logDebug("Wrote %zu bytes to socket", data.size);
                onWriteComplete(data.size);     // notify that data's been written
            }
            logInfo("End of writeLoop; err = %s", c4error_descriptionStr(_socket->error()));
            closeWithError(_socket->error(), "writeLoop");
        } catch (const exception &x) {
            closeWithException(x, "writeLoop");
        }
        release(this);
    }


    void BuiltInWebSocket::closeWithException(const exception &x, const char *where) {
        // Convert exception to CloseStatus:
        logError("caught exception on %s: %s", where, x.what());
        error e = error::convertException(x);
        closeWithError(c4error_make(C4ErrorDomain(e.domain), e.code, slice(e.what())), where);
    }


    void BuiltInWebSocket::closeWithError(C4Error err, const char *where) {
        if (err.code == 0) {
            onClose(0);
        } else {
            alloc_slice message(c4error_getMessage(err));
            CloseStatus status {kUnknownError, err.code, message};
            if (err.domain == WebSocketDomain)
                status.reason = kWebSocketClose;
            else if (err.domain == POSIXDomain)
                status.reason = kPOSIXError;
            else if (err.domain == NetworkDomain)
                status.reason = kNetworkError;
            onClose(status);
        }
    }

} }
