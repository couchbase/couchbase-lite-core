//
//  MockWSProvider.hh
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
    class MockWSConnection;


    /** A nonfunctional WebSocket provider for testing. */
    class MockWSProvider : public Provider {
    public:
        virtual void addProtocol(const std::string &protocol) override {
            _protocols.insert(protocol);
        }

        inline virtual Connection* connect(const Address&, Delegate&) override;

    protected:
        std::set<std::string> _protocols;
    };


    /** A nonfunctional WebSocket connection for testing. */
    class MockWSConnection : public Connection, public Actor {
    public:

        MockWSConnection(MockWSProvider &provider, const Address &address, Delegate &delegate)
        :Connection(provider, delegate)
        {
            enqueue(&MockWSConnection::_start);
        }

        ~MockWSConnection() {
            assert(!_isOpen);
        }

        virtual void close() override {
            enqueue(&MockWSConnection::_close);
        }

        virtual void send(fleece::slice msg, bool binary) override {
            assert(_isOpen);
            enqueue(&MockWSConnection::_send, fleece::alloc_slice(msg), binary);
        }

        // Mock API, to simulate events:

        void simulateConnected() {
            enqueue(&MockWSConnection::_simulateConnected);
        }

        void simulateReceived(fleece::slice message, bool binary =true) {
            enqueue(&MockWSConnection::_simulateReceived, fleece::alloc_slice(message), binary);
        }

        void simulateClosed(int status =1000, const char *reason =nullptr) {
            enqueue(&MockWSConnection::_simulateClosed, status, fleece::alloc_slice(reason));
        }

        void simulateErrored(int code, const char *reason =nullptr) {
            enqueue(&MockWSConnection::_simulateErrored, code, fleece::alloc_slice(reason));
        }

    protected:
        // These can be overridden to change the mock's behavior:

        virtual void _start() {
            _simulateConnected();
        }

        virtual void _close() {
            _simulateClosed(1000, fleece::alloc_slice(fleece::nullslice));
        }

        virtual void _send(fleece::alloc_slice msg, bool binary) {
            Log("WS SEND: %s", formatMsg(msg, binary).c_str());
            delegate().onWebSocketWriteable();
        }

        // These can't be overridden, but subclasses can call them:

        void _simulateConnected() {
            Log("WS CONNECTED");
            assert(!_isOpen);
            _isOpen = true;
            delegate().onWebSocketConnect();
        }

        void _simulateReceived(fleece::alloc_slice msg, bool binary) {
            Log("WS RECEIVED: %s", formatMsg(msg, binary).c_str());
            assert(_isOpen);
            delegate().onWebSocketMessage(msg, binary);
        }

        void _simulateClosed(int status, fleece::alloc_slice reason) {
            Log("WS CLOSED; status=%d", status);
            _isOpen = false;
            delegate().onWebSocketClose(status, reason);
            release(this);
        }

        void _simulateErrored(int code, fleece::alloc_slice reason) {
            Log("WS CLOSED WITH ERROR; code=%d", code);
            _isOpen = false;
            delegate().onWebSocketError(code, reason);
            release(this);
        }


        std::string formatMsg(fleece::slice msg, bool binary) {
            if (binary) {
                std::stringstream desc;
                desc << std::hex;
                for (size_t i = 0; i < msg.size; i++) {
                    desc << std::setw(2) << std::setfill('0') << (unsigned)msg[i];
                    if ((i % 32) == 31)
                        desc << "\n\t\t";
                    else if ((i % 4) == 3)
                        desc << ' ';
                }
                return desc.str();
            } else {
                return std::string((char*)msg.buf, msg.size);
            }
        }

        std::atomic<bool> _isOpen {false};
    };


    inline Connection* MockWSProvider::connect(const Address &address, Delegate &delegate) {
        auto conn = new MockWSConnection(*this, std::move(address), delegate);
        retain(conn);
        return conn;
    }

} }
