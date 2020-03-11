//
// MockProvider.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Error.hh"
#include "Logging.hh"
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>


namespace litecore { namespace websocket {

    /** A nonfunctional WebSocket connection for testing. It simply logs messages.
        The handler methods can be overridden to examine messages or do other things with them. */
    class MockWebSocket : public WebSocket {
    public:
        class Driver;

        MockWebSocket(const fleece::alloc_slice &url)
        :WebSocket(url)
        { }

        virtual Driver* createDriver() {
            return new Driver(this);
        }

        // WebSocket API:

        virtual void connect() override {
            if (!_driver)
                _driver = createDriver();
            _driver->enqueue(&Driver::_connect);
        }

        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) override {
            _driver->enqueue(&Driver::_close, status, fleece::alloc_slice(message));
        }

        virtual bool send(fleece::slice msg, bool binary) override {
            _driver->enqueue(&Driver::_send, fleece::alloc_slice(msg), binary);
            return true; //FIX: Keep track of buffer size
        }


        // Mock API -- call this to simulate incoming events:

        void simulateHTTPResponse(int status, const fleece::AllocedDict &headers,
                                  actor::delay_t latency = actor::delay_t::zero())
        {
            _driver->enqueueAfter(latency, &Driver::_simulateHTTPResponse, status, headers);
        }

        void simulateConnected(actor::delay_t latency = actor::delay_t::zero()) {
            _driver->enqueueAfter(latency, &Driver::_simulateConnected);
        }

        void simulateReceived(fleece::slice message,
                              bool binary =true,
                              actor::delay_t latency = actor::delay_t::zero())
        {
            _driver->enqueueAfter(latency, &Driver::_simulateReceived, fleece::alloc_slice(message), binary);
        }

        void simulateClosed(CloseReason reason =kWebSocketClose,
                            int status =1000,
                            const char *message =nullptr,
                            actor::delay_t latency = actor::delay_t::zero())
        {
            _driver->enqueueAfter(latency,
                         &Driver::_simulateClosed,
                         {reason, status, fleece::alloc_slice(message)});
        }


        Driver* driver() const    {return _driver;}


        class Driver : public actor::Actor, protected Logging {
        public:

            Driver(MockWebSocket *ws)
            :Logging(WSLogDomain)
            ,_webSocket(ws)
            { }

            const std::string& name() const {
                return _webSocket->name;
            }

            virtual std::string loggingIdentifier() const override {
                return name();
            }

            virtual std::string loggingClassName() const override {
                return "MockWS";
            }

        protected:

            ~Driver() {
                DebugAssert(!_isOpen);
            }

            // These can be overridden to change the mock's behavior:

            virtual void _connect() {
                _simulateConnected();
            }

            bool connected() const {
                return _isOpen;
            }

            virtual void _close(int status, fleece::alloc_slice message) {
                _simulateClosed({kWebSocketClose, status, message});
            }

            virtual void _send(fleece::alloc_slice msg, bool binary) {
                if (!_isOpen)
                    return;
                logDebug("SEND: %s", formatMsg(msg, binary).c_str());
                _webSocket->delegate().onWebSocketWriteable();
            }

            virtual void _closed() {
                _webSocket->clearDelegate();
                _webSocket = nullptr;  // breaks cycle
            }

            virtual void _simulateHTTPResponse(int status, fleece::AllocedDict headers) {
                logVerbose("GOT RESPONSE (%d)", status);
                DebugAssert(!_isOpen);
                _webSocket->delegate().onWebSocketGotHTTPResponse(status, headers);
            }

            virtual void _simulateConnected() {
                logVerbose("CONNECTED");
                DebugAssert(!_isOpen);
                _isOpen = true;
                _webSocket->delegate().onWebSocketConnect();
            }

            virtual void _simulateReceived(fleece::alloc_slice msg, bool binary) {
                if (!_isOpen)
                    return;
                logDebug("RECEIVED: %s", formatMsg(msg, binary).c_str());
                _webSocket->delegate().onWebSocketMessage(msg, binary);
            }

            virtual void _simulateClosed(CloseStatus status) {
                if (!_isOpen)
                    return;
                log("Closing with %-s %d: %.*s",
                      status.reasonName(), status.code,
                      (int)status.message.size, status.message.buf);
                _isOpen = false;
                _webSocket->delegate().onWebSocketClose(status);
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

            friend class MockWebSocket;

            Retained<MockWebSocket> _webSocket;
            std::atomic<bool> _isOpen {false};
        };

        
        Retained<Driver> _driver;

        friend class MockProvider;
    };

} }
