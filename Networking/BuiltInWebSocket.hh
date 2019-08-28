//
// BuiltInWebSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketImpl.hh"
#include "TCPSocket.hh"
#include "Channel.hh"
#include <exception>
#include <mutex>
#include <thread>

extern "C" {
    /** Call this to use BuiltInWebSocket as the WebSocket implementation. */
    void C4RegisterBuiltInWebSocket();
}

namespace sockpp {
    class tls_context;
}

namespace litecore { namespace websocket {

    /** WebSocket implementation using TCPSocket. */
    class BuiltInWebSocket : public WebSocketImpl {
    public:
        static void registerWithReplicator();

        BuiltInWebSocket(const URL &url,
                         Role role,
                         const fleece::AllocedDict &options);

        virtual void connect() override;

    protected:
        ~BuiltInWebSocket();

        // Implementations of WebSocketImpl abstract methods:
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice) override;
        virtual void receiveComplete(size_t byteCount) override;
        virtual void requestClose(int status, fleece::slice message) override;

    private:
        void _connect();
        bool configureProxy(net::HTTPLogic&, fleece::Dict proxyOpt);
        std::unique_ptr<net::ClientSocket> _connectLoop()MUST_USE_RESULT;
        void readLoop();
        void writeLoop();
        void closeWithException(const std::exception&, const char *where);
        void closeWithError(C4Error, const char *where);

        size_t readCapacity() const      {return kMaxReceivedBytesPending - _receivedBytesPending;}

        std::unique_ptr<net::TCPSocket> _socket;            // The TCP socket
        std::unique_ptr<sockpp::tls_context> _tlsContext;   // TLS settings
        std::thread _readerThread;                          // Thread that reads bytes in a loop
        std::thread _writerThread;                          // Thread that writes bytes in a loop

        actor::Channel<alloc_slice> _outbox;                // Data waiting to be sent by writer
        
        // Max number of bytes read that haven't been processed by the client yet.
        // Beyond this point, I will stop reading from the socket, sending
        // backpressure to the peer.
        static constexpr size_t kMaxReceivedBytesPending = 100 * 1024;

        size_t _receivedBytesPending = 0;                   // # bytes read but not handled yet
        std::mutex _receiveMutex;                           // Locking for receives
        std::condition_variable _receiveCond;
    };


} }
