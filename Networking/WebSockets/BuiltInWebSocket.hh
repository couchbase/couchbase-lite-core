//
// BuiltInWebSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketImpl.hh"
#include "TCPSocket.hh"
#include "HTTPLogic.hh"
#include "c4Base.hh"
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
    /** Call this to use BuiltInWebSocket as the WebSocket implementation. */
    void C4RegisterBuiltInWebSocket();
}

namespace litecore::crypto {
    struct Identity;
}

namespace litecore { namespace websocket {

    /** WebSocket implementation using TCPSocket. */
    class BuiltInWebSocket final : public WebSocketImpl, public net::HTTPLogic::CookieProvider {
    public:
        /** This must be called once, for c4Replicator to use BuiltInWebSocket by default. */
        static void registerWithReplicator();

        /** Client-side constructor. Call \ref connect() afterwards. */
        BuiltInWebSocket(const URL &url,
                         const Parameters&,
                         C4Database *database);

        /** Server-side constructor; takes an already-connected socket that's been through the
            HTTP WebSocket handshake and is ready to send/receive frames. */
        BuiltInWebSocket(const URL &url,
                         std::unique_ptr<net::ResponderSocket>);

        /** Starts the TCP connection for a client socket. */
        virtual void connect() override;
        
    protected:
        ~BuiltInWebSocket();

        // Implementations of WebSocketImpl abstract methods:
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice) override;
        virtual void receiveComplete(size_t byteCount) override;
        virtual void requestClose(int status, fleece::slice message) override;

        // CookieProvider API:
        virtual fleece::alloc_slice cookiesForRequest(const net::Address&) override;
        virtual void setCookie(const net::Address&, fleece::slice cookieHeader) override;

    private:
        BuiltInWebSocket(const URL&, Role, const Parameters &);
        void _bgConnect();
        void setThreadName();
        bool configureClientCert(fleece::Dict auth);
        bool configureProxy(net::HTTPLogic&, fleece::Dict proxyOpt);
        std::unique_ptr<net::ClientSocket> _connectLoop() MUST_USE_RESULT;
        void ioLoop();
        void awaitReadable();
        void awaitWriteable();
        void readFromSocket();
        void writeToSocket();
        void closeWithException(const std::exception&, const char *where);
        void closeWithError(C4Error);

        // Max number of bytes read that haven't been processed by the client yet.
        // Beyond this point, I will stop reading from the socket, sending
        // backpressure to the peer.
        static constexpr size_t kReadCapacity = 64 * 1024;

        // Size of the buffer allocated for reading from the socket.
        static constexpr size_t kReadBufferSize = 32 * 1024;

        Retained<C4Database> _database;                     // The database (used only for cookies)
        std::unique_ptr<net::TCPSocket> _socket;            // The TCP socket
        Retained<BuiltInWebSocket> _selfRetain;             // Keeps me alive while connected
        Retained<net::TLSContext> _tlsContext;              // TLS settings
        std::thread _connectThread;                         // Thread that opens the connection

        std::vector<fleece::slice> _outbox;                 // Byte ranges to be sent by writer
        std::vector<fleece::alloc_slice> _outboxAlloced;    // Same, but retains the heap data
        std::mutex _outboxMutex;                            // Locking for outbox

        std::atomic<size_t> _curReadCapacity {kReadCapacity}; // # bytes I can read from socket
        fleece::alloc_slice _readBuffer;                    // Buffer used by readFromSocket().
    };

} }
