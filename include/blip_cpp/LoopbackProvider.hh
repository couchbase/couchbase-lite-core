//
//  LoopbackProvider.hh
//  blip_cpp
//
//  Created by Jens Alfke on 2/18/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "MockProvider.hh"
#include <atomic>


namespace litecore { namespace websocket {
    class LoopbackProvider;


    /** A WebSocket connection that relays messages to another instance of LoopbackConnection. */
    class LoopbackConnection : public MockConnection {
    public:

        void connectToPeer(LoopbackConnection *peer) {
            assert(peer);
            enqueue(&LoopbackConnection::_connectToPeer, Retained<LoopbackConnection>(peer));
        }

        virtual void send(fleece::slice msg, bool binary) override {
            _bufferedBytes += msg.size;
            MockConnection::send(msg, binary);
        }

        virtual void ack(size_t msgSize) {
            enqueue(&LoopbackConnection::_ack, msgSize);
        }

    protected:
        friend class LoopbackProvider;

        LoopbackConnection(MockProvider &provider, const Address &address, double latency)
        :MockConnection(provider, address)
        ,_latency(latency)
        { }

        virtual void _connect() override {
            if (_peer)
                MockConnection::_connect();
        }

        virtual void _connectToPeer(Retained<LoopbackConnection> peer) {
            if (peer != _peer) {
                assert(!_peer);
                _peer = peer;
                _simulateConnected();
            }
        }

        virtual void _send(fleece::alloc_slice msg, bool binary) override {
            LogTo(WSMock, "%s SEND: %s", name.c_str(), formatMsg(msg, binary).c_str());
            assert(_peer);
            _peer->simulateReceived(msg, binary, _latency);
        }

        virtual void _simulateReceived(fleece::alloc_slice msg, bool binary) override {
            MockConnection::_simulateReceived(msg, binary);
            _peer->ack(msg.size);
        }

        virtual void _ack(size_t msgSize) {
            auto bufSize = (_bufferedBytes -= msgSize);
            if (bufSize == 0) {
                LogTo(WSMock, "%s WRITEABLE", name.c_str());
                delegate().onWebSocketWriteable();
            }
        }

        virtual void _close(int status, fleece::alloc_slice message) override {
            std::string messageStr(message);
            LogTo(WSMock, "%s CLOSE; status=%d", name.c_str(), status);
            assert(_peer);
            _peer->simulateClosed(status, messageStr.c_str(), _latency);
            MockConnection::_close(status, message);
        }

        virtual void _closed() override {
            _peer = nullptr;
            MockConnection::_closed();
        }

    private:
        double _latency {0.0};
        Retained<LoopbackConnection> _peer;
        std::atomic<size_t> _bufferedBytes {0};
    };


    /** A WebSocketProvider that creates pairs of Connection objects that talk to each other. */
    class LoopbackProvider : public MockProvider {
    public:
        /** Constructs a WebSocketProvider. A latency time can be provided, which is the delay
            before a message sent by one connection is received by its peer. */
        LoopbackProvider(double latency = 0.0)
        :_latency(latency)
        { }

        LoopbackConnection* createConnection(const Address &address) override {
            return new LoopbackConnection(*this, address, _latency);
        }

        /** Connects two LoopbackConnection objects to each other, so each receives messages sent
            by the other. When one closes, the other will receive a close event. */
        void connect(Connection *c1, Connection *c2) {
            auto lc1 = dynamic_cast<LoopbackConnection*>(c1);
            auto lc2 = dynamic_cast<LoopbackConnection*>(c2);
            lc1->connectToPeer(lc2);
            lc2->connectToPeer(lc1);
        }

    private:
        double _latency {0.0};
    };


} }
