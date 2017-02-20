//
//  MockProvider.hh
//  blip_cpp
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Logging.hh"
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>


namespace litecore { namespace websocket {

    /** A nonfunctional WebSocket connection for testing. It simply logs messages.
        The handler methods can be overridden to examine messages or do other things with them. */
    class MockWebSocket : public WebSocket, public Actor {
    public:

        ~MockWebSocket() {
            assert(!_isOpen);
        }

        virtual void connect() override {
            enqueue(&MockWebSocket::_connect);
        }

        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) override {
            enqueue(&MockWebSocket::_close, status, fleece::alloc_slice(message));
        }

        virtual void send(fleece::slice msg, bool binary) override {
            assert(_isOpen);
            enqueue(&MockWebSocket::_send, fleece::alloc_slice(msg), binary);
        }

        // Mock API -- call this to simulate incoming events:

        void simulateConnected(double latency =0.0) {
            enqueueAfter(latency, &MockWebSocket::_simulateConnected);
        }

        void simulateReceived(fleece::slice message, bool binary =true, double latency =0.0) {
            enqueueAfter(latency, &MockWebSocket::_simulateReceived, fleece::alloc_slice(message), binary);
        }

        void simulateClosed(int status =1000, const char *reason =nullptr, double latency =0.0) {
            enqueueAfter(latency, &MockWebSocket::_simulateClosed, status, fleece::alloc_slice(reason));
        }

        void simulateErrored(int code, const char *reason =nullptr, double latency =0.0) {
            enqueueAfter(latency, &MockWebSocket::_simulateErrored, code, fleece::alloc_slice(reason));
        }

    protected:
        friend class MockProvider;

        static LogDomain WSMock;

        MockWebSocket(Provider &provider, const Address &address)
        :WebSocket(provider, address)
        {
            retain(this);   // balanced by release in _closed
        }

        // These can be overridden to change the mock's behavior:

        virtual void _connect() {
            _simulateConnected();
        }

        virtual void _close(int status, fleece::alloc_slice message) {
            _simulateClosed(status, message);
        }

        virtual void _send(fleece::alloc_slice msg, bool binary) {
            LogTo(WSMock, "%s SEND: %s", name.c_str(), formatMsg(msg, binary).c_str());
            delegate().onWebSocketWriteable();
        }

        virtual void _closed() {
            release(this);
        }

        virtual void _simulateConnected() {
            LogTo(WSMock, "%s CONNECTED", name.c_str());
            assert(!_isOpen);
            _isOpen = true;
            delegate().onWebSocketConnect();
        }

        virtual void _simulateReceived(fleece::alloc_slice msg, bool binary) {
            LogTo(WSMock, "%s RECEIVED: %s", name.c_str(), formatMsg(msg, binary).c_str());
            assert(_isOpen);
            delegate().onWebSocketMessage(msg, binary);
        }

        virtual void _simulateClosed(int status, fleece::alloc_slice reason) {
            LogTo(WSMock, "%s CLOSED; status=%d", name.c_str(), status);
            _isOpen = false;
            delegate().onWebSocketClose(status, reason);
            _closed();
        }

        virtual void _simulateErrored(int code, fleece::alloc_slice reason) {
            LogTo(WSMock, "%s CLOSED WITH ERROR; code=%d", name.c_str(), code);
            _isOpen = false;
            delegate().onWebSocketError(code, reason);
            _closed();
        }


        std::string formatMsg(fleece::slice msg, bool binary, size_t maxBytes = 64) {
            std::stringstream desc;
            size_t size = std::min(msg.size, maxBytes);

            if (binary) {
                desc << std::hex;
                for (size_t i = 0; i < size; i++) {
                    if (i > 0) {
                        if ((i % 32) == 0)
                            desc << "\n\t\t";
                        else if ((i % 4) == 0)
                            desc << ' ';
                    }
                    desc << std::setw(2) << std::setfill('0') << (unsigned)msg[i];
                }
                desc << std::dec;
            } else {
                desc.write((char*)msg.buf, size);
            }

            if (size < msg.size)
                desc << "... [" << msg.size << "]";
            return desc.str();
        }

        std::atomic<bool> _isOpen {false};
    };


    /** A nonfunctional WebSocket provider for testing. */
    class MockProvider : public Provider {
    public:
        virtual void addProtocol(const std::string &protocol) override {
            _protocols.insert(protocol);
        }

        virtual WebSocket* createWebSocket(const Address &address) override {
            return new MockWebSocket(*this, address);
        }

    protected:
        std::set<std::string> _protocols;
    };


    LogDomain MockWebSocket::WSMock("WSMock");

} }
