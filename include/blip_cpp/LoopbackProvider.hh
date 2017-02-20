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


    /** A WebSocket connection that relays messages to another instance of LoopbackWebSocket. */
    class LoopbackWebSocket : public MockWebSocket {
    public:

        void connectToPeer(LoopbackWebSocket *peer) {
            assert(peer);
            enqueue(&LoopbackWebSocket::_connectToPeer, Retained<LoopbackWebSocket>(peer));
        }

        virtual void send(fleece::slice msg, bool binary) override {
            _bufferedBytes += msg.size;
            MockWebSocket::send(msg, binary);
        }

        virtual void ack(size_t msgSize) {
            enqueue(&LoopbackWebSocket::_ack, msgSize);
        }

    protected:
        friend class LoopbackProvider;

        LoopbackWebSocket(MockProvider &provider, const Address &address, double latency)
        :MockWebSocket(provider, address)
        ,_latency(latency)
        { }

        virtual void _connect() override {
            if (_peer)
                MockWebSocket::_connect();
        }

        virtual void _connectToPeer(Retained<LoopbackWebSocket> peer) {
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
            MockWebSocket::_simulateReceived(msg, binary);
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
            MockWebSocket::_close(status, message);
        }

        virtual void _closed() override {
            _peer = nullptr;
            MockWebSocket::_closed();
        }

    private:
        double _latency {0.0};
        Retained<LoopbackWebSocket> _peer;
        std::atomic<size_t> _bufferedBytes {0};
    };


    /** A WebSocketProvider that creates pairs of WebSocket objects that talk to each other. */
    class LoopbackProvider : public MockProvider {
    public:
        /** Constructs a WebSocketProvider. A latency time can be provided, which is the delay
            before a message sent by one connection is received by its peer. */
        LoopbackProvider(double latency = 0.0)
        :_latency(latency)
        { }

        LoopbackWebSocket* createWebSocket(const Address &address) override {
            return new LoopbackWebSocket(*this, address, _latency);
        }

        /** Connects two LoopbackWebSocket objects to each other, so each receives messages sent
            by the other. When one closes, the other will receive a close event. */
        void connect(WebSocket *c1, WebSocket *c2) {
            auto lc1 = dynamic_cast<LoopbackWebSocket*>(c1);
            auto lc2 = dynamic_cast<LoopbackWebSocket*>(c2);
            lc1->connectToPeer(lc2);
            lc2->connectToPeer(lc1);
        }

    private:
        double _latency {0.0};
    };


} }
