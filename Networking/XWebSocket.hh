//
// XWebSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketImpl.hh"
#include "XSocket.hh"
#include "Channel.hh"
#include <exception>
#include <mutex>
#include <thread>

extern "C" {
    void C4RegisterXWebSocket();
}

namespace litecore { namespace websocket {

    class XWebSocket : public WebSocketImpl {
    public:
        static void registerWithReplicator();

        XWebSocket(const URL &url,
                   Role role,
                   const fleece::AllocedDict &options);

        virtual void connect() override;

    protected:
        ~XWebSocket();

        // Implementations of WebSocketImpl abstract methods:
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice) override;
        virtual void receiveComplete(size_t byteCount) override;
        virtual void requestClose(int status, fleece::slice message) override;

    private:
        void _connect();
        void readLoop();
        void writeLoop();
        void closeWithError(const std::exception&);

        size_t readCapacity() const      {return kMaxReceivedBytesPending - _receivedBytesPending;}

        net::XSocket _socket;
        std::thread _readerThread;
        std::thread _writerThread;

        actor::Channel<alloc_slice> _outbox;
        
        // Max number of bytes read that haven't been processed by the client yet.
        // Beyond this point, I will stop reading from the socket, sending backpressure to the peer.
        static constexpr size_t kMaxReceivedBytesPending = 100 * 1024;

        size_t _receivedBytesPending = 0;
        std::mutex _receiveMutex;
        std::condition_variable _receiveCond;
    };


} }
