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
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
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


    XWebSocket::XWebSocket(const URL &url,
                           Role role,
                           const fleece::AllocedDict &options)
    :WebSocketImpl(url, role, options, true)
    ,_socket(repl::Address(url))
    { }


    XWebSocket::~XWebSocket() {
        // This could be called from various threads, including the reader...
        if (_readerThread.joinable())
            _readerThread.detach();
    }


    void XWebSocket::connect() {
        // Spawn a thread to connect and run the read loop:
        retain(this);
        _readerThread = thread([&]() {_connect();});
    }


    void XWebSocket::closeSocket() {
        logVerbose("XWEBSOCKET: closeSocket");
        _socket.close();
        
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



    void XWebSocket::_connect() {
        try {
            // Connect:
            _socket.connect();

            // WebSocket handshake:
            Dict headers = options()[kC4ReplicatorOptionExtraHeaders].asDict();
            string protocol = string(options()[kC4SocketOptionWSProtocols].asString());
            string nonce = _socket.sendWebSocketRequest(headers, protocol);
            auto response = _socket.readHTTPResponse();
            gotHTTPResponse(response.status, response.headers);

            CloseStatus status;
            if (!_socket.checkWebSocketResponse(response, nonce, protocol, status)) {
                delegate().onWebSocketClose(status);
                release(this);
                return;
            }
            
            onConnect();
        } catch (exception &x) {
            CloseStatus status;
            // TODO: populate
            delegate().onWebSocketClose(status);
            return;
        }

        // OK, now we are connected -- start the loops for I/O:
        retain(this);
        _writerThread = thread([&]() {writeLoop();});
        readLoop();
    }


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
                slice data = _socket.read(capacity);
                logVerbose("XWEBSOCKET: Received %zu bytes from socket", data.size);
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
            logInfo("XWEBSOCKET: EOF on readLoop");
            onClose(0);

        } catch (const exception &x) {
            closeWithError(x);
        }
        _writerThread.join();
        release(this);
    }


    void XWebSocket::writeLoop() {
        try {
            while (true) {
                alloc_slice data = _outbox.pop();
                if (_socket.write_n(data) == 0)
                    break;
                logVerbose("XWEBSOCKET: Wrote %zu bytes to socket", data.size);
                onWriteComplete(data.size);     // notify that data's been written
            }
            logVerbose("EOF on writeLoop");
        } catch (const exception &x) {
            closeWithError(x);
        }
        release(this);
    }


    void XWebSocket::closeWithError(const exception &x) {
        error e = error::convertException(x);
        logInfo("XWEBSOCKET: caught exception: %s", e.what());
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
