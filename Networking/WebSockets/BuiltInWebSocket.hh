//
// BuiltInWebSocket.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "WebSocketImpl.hh"
#include "TCPSocket.hh"
#include "HTTPLogic.hh"
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#ifdef LITECORE_PERF_TESTING_MODE
CBL_CORE_API
#endif
/** Call this to use BuiltInWebSocket as the WebSocket implementation. */
void C4RegisterBuiltInWebSocket();
}

namespace litecore::crypto {
    struct Identity;
}

namespace litecore::repl {
    class DBAccess;
}

namespace litecore::websocket {

    /** WebSocket implementation using TCPSocket. */
    class BuiltInWebSocket final
        : public WebSocketImpl
        , public net::HTTPLogic::CookieProvider {
      public:
        /** Client-side constructor. Call \ref connect() afterwards. */
        BuiltInWebSocket(const URL& url, const Parameters&, std::shared_ptr<repl::DBAccess> database);

        /** Server-side constructor; takes an already-connected socket that's been through the
            HTTP WebSocket handshake and is ready to send/receive frames. */
        BuiltInWebSocket(const URL& url, const Parameters&, std::unique_ptr<net::ResponderSocket>);

        /** Starts the TCP connection for a client socket. */
        void connect() override;

        std::pair<int, Headers> httpResponse() const override;

      protected:
        ~BuiltInWebSocket() override;

        // Implementations of WebSocketImpl abstract methods:
        void closeSocket() override;
        void sendBytes(fleece::alloc_slice) override;
        void receiveComplete(size_t byteCount) override;
        void requestClose(int status, fleece::slice message) override;

        // CookieProvider API:
        fleece::alloc_slice cookiesForRequest(const net::Address&) override;
        void                setCookie(const net::Address&, fleece::slice cookieHeader) override;

      private:
        BuiltInWebSocket(const URL&, Role, const Parameters&);
        void                                             _bgConnect();
        void                                             setThreadName();
        static bool                                      configureAuthHeader(net::HTTPLogic&, fleece::Dict auth);
        static bool                                      configureProxy(net::HTTPLogic&, fleece::Dict proxyOpt);
        [[nodiscard]] std::unique_ptr<net::ClientSocket> _connectLoop();
        void                                             ioLoop();
        void                                             awaitReadable();
        void                                             awaitWriteable();
        void                                             readFromSocket();
        void                                             writeToSocket();
        void                                             closeWithException(const std::exception&, const char* where);
        void                                             closeWithError(C4Error);

        // Max number of bytes read that haven't been processed by the client yet.
        // Beyond this point, I will stop reading from the socket, sending
        // backpressure to the peer.
        static constexpr size_t kReadCapacity = 64 * 1024;

        // Size of the buffer allocated for reading from the socket.
        static constexpr size_t kReadBufferSize = 32 * 1024;

        std::shared_ptr<repl::DBAccess> _database;       // The database (used only for cookies)
        std::unique_ptr<net::TCPSocket> _socket;         // The TCP socket
        Retained<BuiltInWebSocket>      _selfRetain;     // Keeps me alive while connected
        Retained<net::TLSContext>       _tlsContext;     // TLS settings
        std::thread                     _connectThread;  // Thread that opens the connection

        int     _responseStatus = 0;
        Headers _responseHeaders;

        std::vector<fleece::slice>       _outbox;         // Byte ranges to be sent by writer
        std::vector<fleece::alloc_slice> _outboxAlloced;  // Same, but retains the heap data
        std::mutex                       _outboxMutex;    // Locking for outbox

        std::atomic<size_t> _curReadCapacity{kReadCapacity};  // # bytes I can read from socket
        fleece::alloc_slice _readBuffer;                      // Buffer used by readFromSocket().
    };

}  // namespace litecore::websocket
