//
// c4Socket+Internal.hh
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
#include "c4Socket.hh"
#include "WebSocketImpl.hh"

struct c4Database;


namespace litecore { namespace repl {

    // Main factory function to create a WebSocket.
    fleece::Retained<websocket::WebSocket> CreateWebSocket(websocket::URL,
                                                           fleece::alloc_slice options,
                                                           C4Database* NONNULL,
                                                           const C4SocketFactory*,
                                                           void *nativeHandle =nullptr);

    // Returns the WebSocket object associated with a C4Socket
    websocket::WebSocket* WebSocketFrom(C4Socket *c4sock);


    /** Implementation of C4Socket */
    class C4SocketImpl final : public websocket::WebSocketImpl, public C4Socket {
    public:
        static const C4SocketFactory& registeredFactory();

        using InternalFactory = websocket::WebSocketImpl* (*)(websocket::URL,
                                                              fleece::alloc_slice options,
                                                              C4Database* NONNULL);
        static void registerInternalFactory(InternalFactory);

        static Parameters convertParams(fleece::slice c4SocketOptions);

        C4SocketImpl(websocket::URL,
                     websocket::Role,
                     fleece::alloc_slice options,
                     const C4SocketFactory*,
                     void *nativeHandle =nullptr);

        ~C4SocketImpl();

        void closeWithException();

        // WebSocket publiv API:
        void connect() override;

        // C4Socket API:
        void gotHTTPResponse(int httpStatus, slice responseHeadersFleece) override;
        void opened() override;
        void closed(C4Error errorIfAny) override;
        void closeRequested(int status, slice message) override;
        void completedWrite(size_t byteCount) override;
        void received(slice data) override;

    protected:
        // WebSocket protected API:
        virtual void requestClose(int status, fleece::slice message) override;
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice bytes) override;
        virtual void receiveComplete(size_t byteCount) override;

    private:
        C4SocketFactory const _factory;
    };

} }
