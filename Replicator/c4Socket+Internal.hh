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
#include "c4Socket.h"
#include "WebSocketImpl.hh"

struct c4Database;


namespace litecore { namespace repl {

    // Returns the WebSocket object associated with a C4Socket
    websocket::WebSocket* WebSocketFrom(C4Socket *c4sock);


    /** Implementation of C4Socket */
    class C4SocketImpl : public websocket::WebSocketImpl, public C4Socket {
    public:
        static void registerFactory(const C4SocketFactory&);
        static const C4SocketFactory& registeredFactory();

        C4SocketImpl(websocket::URL,
                     websocket::Role role,
                     fleece::alloc_slice options,
                     const C4SocketFactory *factory_,
                     void *nativeHandle_ =nullptr);

        ~C4SocketImpl();

        void connect() override;

    protected:
        virtual void requestClose(int status, fleece::slice message) override;
        virtual void closeSocket() override;
        virtual void sendBytes(fleece::alloc_slice bytes) override;
        virtual void receiveComplete(size_t byteCount) override;

    private:
        C4SocketFactory const _factory;

        static void validateFactory(const C4SocketFactory&);

        static C4SocketFactory *sRegisteredFactory;
    };

} }
