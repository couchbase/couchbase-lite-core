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
#include <chrono>


namespace litecore { namespace websocket {
    class LoopbackProvider;

    static constexpr size_t kSendBufferSize = 32 * 1024;


    /** A WebSocket connection that relays messages to another instance of LoopbackWebSocket. */
    class LoopbackWebSocket : public MockWebSocket {
    public:
        virtual void connect() override {
            assert(_peer);
            MockWebSocket::connect();
        }

        virtual bool send(fleece::slice msg, bool binary) override {
            auto newValue = (_bufferedBytes += msg.size);
            MockWebSocket::send(msg, binary);
            return newValue <= kSendBufferSize;
        }

        virtual void ack(size_t msgSize) {
            enqueue(&LoopbackWebSocket::_ack, msgSize);
        }

    protected:
        friend class LoopbackProvider;

        LoopbackWebSocket(MockProvider &provider, const Address &address, actor::delay_t latency)
        :MockWebSocket(provider, address)
        ,_latency(latency)
        { }

        void bind(LoopbackWebSocket *peer, const fleeceapi::AllocedDict &responseHeaders) {
            // Called by LoopbackProvider::bind, which is called before my connect() method,
            // so it's safe to set the member variables directly instead of on the actor queue.
            _peer = peer;
            _responseHeaders = responseHeaders;
        }

        virtual void _connect() override {
            _simulateHTTPResponse(200, _responseHeaders);
            MockWebSocket::_connect();
        }

        virtual void _send(fleece::alloc_slice msg, bool binary) override {
            if (_peer) {
                LogDebug(WSMock, "%s SEND: %s", name.c_str(), formatMsg(msg, binary).c_str());
                _peer->simulateReceived(msg, binary, _latency);
            } else {
                LogTo(WSMock, "%s SEND: Failed, socket is closed", name.c_str());
            }
        }

        virtual void _simulateReceived(fleece::alloc_slice msg, bool binary) override {
            MockWebSocket::_simulateReceived(msg, binary);
            _peer->ack(msg.size);
        }

        virtual void _ack(size_t msgSize) {
            if (!connected())
                return;
            auto newValue = (_bufferedBytes -= msgSize);
            if (newValue <= kSendBufferSize && newValue + msgSize > kSendBufferSize) {
                LogVerbose(WSMock, "%s WRITEABLE", name.c_str());
                delegate().onWebSocketWriteable();
            }
        }

        virtual void _close(int status, fleece::alloc_slice message) override {
            std::string messageStr(message);
            LogTo(WSMock, "%s CLOSE; status=%d", name.c_str(), status);
            assert(_peer);
            _peer->simulateClosed(kWebSocketClose, status, messageStr.c_str(), _latency);
            MockWebSocket::_close(status, message);
        }

        virtual void _closed() override {
            _peer = nullptr;
            MockWebSocket::_closed();
        }

    private:
        actor::delay_t _latency {0.0};
        Retained<LoopbackWebSocket> _peer;
        fleeceapi::AllocedDict _responseHeaders;
        std::atomic<size_t> _bufferedBytes {0};
    };


    /** A WebSocketProvider that creates pairs of WebSocket objects that talk to each other. */
    class LoopbackProvider : public MockProvider {
    public:

        /** Constructs a WebSocketProvider. A latency time can be provided, which is the delay
            before a message sent by one connection is received by its peer. */
        LoopbackProvider(actor::delay_t latency = actor::delay_t::zero())
        :_latency(latency)
        { }

        LoopbackWebSocket* createWebSocket(const Address &address,
                                           const fleeceapi::AllocedDict &options ={}) override {
            return new LoopbackWebSocket(*this, address, _latency);
        }

        /** Binds two LoopbackWebSocket objects to each other, so after they open, each will 
            receive messages sent by the other. When one closes, the other will receive a close
            event.
            MUST be called before the socket objects' connect() methods are called! */
        void bind(WebSocket *c1, WebSocket *c2,
                  const fleeceapi::AllocedDict &responseHeaders ={})
        {
            auto lc1 = dynamic_cast<LoopbackWebSocket*>(c1);
            auto lc2 = dynamic_cast<LoopbackWebSocket*>(c2);
            lc1->bind(lc2, responseHeaders);
            lc2->bind(lc1, responseHeaders);
        }

    private:
        actor::delay_t _latency {0.0};
    };


} }
