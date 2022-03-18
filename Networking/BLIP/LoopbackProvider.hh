//
// LoopbackProvider.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Headers.hh"
#include "Actor.hh"
#include "Error.hh"
#include "Logging.hh"
#include "NumConversion.hh"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <cinttypes>
#include <deque>

namespace litecore { namespace websocket {
    class LoopbackProvider;

    static constexpr size_t kSendBufferSize = 256 * 1024;


    /** A WebSocket connection that relays messages to another instance of LoopbackWebSocket. */
    class LoopbackWebSocket final : public WebSocket {
    protected:
        class Driver;
    private:
        Retained<Driver> _driver;
        const actor::delay_t _latency;

    public:

        LoopbackWebSocket(const fleece::alloc_slice &url,
                          Role role,
                          actor::delay_t latency =actor::delay_t::zero())
        :WebSocket(url, role)
        ,_latency(latency)
        { }

        /** Binds two LoopbackWebSocket objects to each other, so after they open, each will
            receive messages sent by the other. When one closes, the other will receive a close
            event.
            MUST be called before the socket objects' connect() methods are called! */
        static void bind(WebSocket *c1, WebSocket *c2,
                         const websocket::Headers &responseHeaders ={})
        {
            auto lc1 = dynamic_cast<LoopbackWebSocket*>(c1);
            auto lc2 = dynamic_cast<LoopbackWebSocket*>(c2);
            lc1->bind(lc2, responseHeaders);
            lc2->bind(lc1, responseHeaders);
        }

        virtual void connect() override {
            Assert(_driver && _driver->_peer);
            _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_connect));
        }

        virtual bool send(fleece::slice msg, bool binary) override {
            auto newValue = (_driver->_bufferedBytes += msg.size);
            _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_send), fleece::alloc_slice(msg), binary);
            return newValue <= kSendBufferSize;
        }

        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) override {
            _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_close), status, fleece::alloc_slice(message));
        }


    protected:

        void bind(LoopbackWebSocket *peer, const websocket::Headers &responseHeaders) {
            Assert(!_driver);
            _driver = createDriver();
            _driver->bind(peer, responseHeaders);
        }

        virtual Driver* createDriver() {
            return new Driver(this, _latency);
        }

        Driver* driver() const    {return _driver;}

        void peerIsConnecting(actor::delay_t latency = actor::delay_t::zero()) {
            _driver->enqueueAfter(latency, FUNCTION_TO_QUEUE(Driver::_peerIsConnecting));
        }

        virtual void ack(size_t msgSize) {
            _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_ack), msgSize);
        }

        void received(Message *message,
                      actor::delay_t latency = actor::delay_t::zero())
        {
            if (latency == actor::delay_t::zero()) {
                _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_received), retained(message));
            } else {
                _driver->enqueue(FUNCTION_TO_QUEUE(Driver::_queueMessage), retained(message));
                _driver->enqueueAfter(latency, FUNCTION_TO_QUEUE(Driver::_dequeueMessage));
            }
        }

        void closed(CloseReason reason =kWebSocketClose,
                    int status =1000,
                    const char *message =nullptr,
                    actor::delay_t latency = actor::delay_t::zero())
        {
            _driver->enqueueAfter(latency,
                                  FUNCTION_TO_QUEUE(Driver::_closed),
                                  CloseStatus(reason, status, fleece::slice(message)));
        }


        class LoopbackMessage : public Message {
        public:
            template <class SLICE>
            LoopbackMessage(LoopbackWebSocket *ws, SLICE data, bool binary)
            :Message(data, binary)
            ,_size(data.size)
            ,_webSocket(ws)
            { }

            ~LoopbackMessage() {
                _webSocket->ack(_size);
            }

        private:
            size_t _size;
            Retained<LoopbackWebSocket> _webSocket;
        };


        // The internal Actor that does the real work
        class Driver final : public actor::Actor {
        public:

            Driver(LoopbackWebSocket *ws, actor::delay_t latency)
            :Actor(WSLogDomain)
            ,_webSocket(ws)
            ,_latency(latency)
            { }

            virtual std::string loggingIdentifier() const override {
                return _webSocket ? _webSocket->name() : "[Already closed]";
            }

            virtual std::string loggingClassName() const override {
                return "LoopbackWS";
            }

            void bind(LoopbackWebSocket *peer, const websocket::Headers &responseHeaders) {
                // Called by LoopbackProvider::bind, which is called before my connect() method,
                // so it's safe to set the member variables directly instead of on the actor queue.
                _peer = peer;
                _responseHeaders = responseHeaders;
            }

            bool connected() const {
                return _state == State::connected;
            }

        protected:

            enum class State {
                unconnected,
                peerConnecting,
                connecting,
                connected,
                closed
            };

            ~Driver() {
                DebugAssert(!connected());
            }

            virtual void _connect() {
                // Connecting uses a handshake, to ensure both sides have notified their delegates
                // they're connected before either side sends a message. In other words, to
                // prevent one side from receiving a message from the peer before it's ready.
                logVerbose("Connecting to peer...");
                Assert(_state < State::connecting);
                _peer->peerIsConnecting(_latency);
                if (_state == State::peerConnecting)
                    connectCompleted();
                else
                    _state = State::connecting;
            }

            void _peerIsConnecting() {
                logVerbose("(Peer is connecting...)");
                switch (_state) {
                    case State::unconnected:
                        _state = State::peerConnecting;
                        break;
                    case State::connecting:
                        connectCompleted();
                        break;
                    case State::closed:
                        // ignore in this state
                        break;
                    default:
                        Assert(false, "illegal state");
                        break;
                }
            }

            void connectCompleted() {
                logInfo("CONNECTED");
                _state = State::connected;
                _webSocket->delegate().onWebSocketGotHTTPResponse(200, _responseHeaders);
                _webSocket->delegate().onWebSocketConnect();
            }

            virtual void _send(fleece::alloc_slice msg, bool binary) {
                if (_peer) {
                    Assert(_state == State::connected);
                    logDebug("SEND: %s", formatMsg(msg, binary).c_str());
                    Retained<Message> message(new LoopbackMessage(_webSocket, msg, binary));
                    _peer->received(message, _latency);
                } else {
                    logInfo("SEND: Failed, socket is closed");
                }
            }

            void _queueMessage(Retained<Message> message) {
                _msgWaitBuffer.push_back(message);
            }

            void _dequeueMessage() {
                Assert(_msgWaitBuffer.size() > 0);
                Retained<Message> msg = _msgWaitBuffer.front();
                _msgWaitBuffer.pop_front();
                _received(msg);
            }

            virtual void _received(Retained<Message> message) {
                if (!connected())
                    return;
                logDebug("RECEIVED: %s", formatMsg(message->data, message->binary).c_str());
                _webSocket->delegate().onWebSocketMessage(message);
            }

            virtual void _ack(size_t msgSize) {
                if (!connected())
                    return;
                auto newValue = (_bufferedBytes -= msgSize);
                if (newValue <= kSendBufferSize && newValue + msgSize > kSendBufferSize) {
                    logDebug("WRITEABLE");
                    _webSocket->delegate().onWebSocketWriteable();
                }
            }

            virtual void _close(int status, fleece::alloc_slice message) {
                if (_state != State::unconnected) {
                    Assert(_state == State::connecting || _state == State::connected);
                    logInfo("CLOSE; status=%d", status);
                    std::string messageStr(message);
                    if (_peer)
                        _peer->closed(kWebSocketClose, status, messageStr.c_str(), _latency);
                }
                _closed({kWebSocketClose, status, message});
            }

            virtual void _closed(CloseStatus status) {
                if (_state == State::closed)
                    return;
                if (_state >= State::connecting) {
                    logInfo("CLOSED with %-s %d: %.*s",
                        status.reasonName(), status.code,
                        fleece::narrow_cast<int>(status.message.size), 
			(char *)status.message.buf);
                    _webSocket->delegate().onWebSocketClose(status);
                } else {
                    logInfo("CLOSED");
                }
                _state = State::closed;
                _peer = nullptr;
                _webSocket->clearDelegate();
                _webSocket = nullptr;  // breaks cycle
            }


            static std::string formatMsg(fleece::slice msg, bool binary, size_t maxBytes = 64) {
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

        private:
            friend class LoopbackWebSocket;

            Retained<LoopbackWebSocket> _webSocket;
            const actor::delay_t _latency {0.0};
            Retained<LoopbackWebSocket> _peer;
            websocket::Headers _responseHeaders;
            std::atomic<size_t> _bufferedBytes {0};
            State _state {State::unconnected};
            std::deque<Retained<Message>> _msgWaitBuffer;
        };
    };

} }
