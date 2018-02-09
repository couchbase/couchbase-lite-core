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


namespace litecore { namespace websocket {

    // Address conversion utilities:
    C4Address c4AddressFrom(const Address &address);
    websocket::Address addressFrom(const C4Address &addr);
    websocket::Address addressFrom(const C4Address &addr, fleece::slice remoteDatabaseName);
    websocket::Address addressFrom(c4Database* otherDB);

    // Returns the WebSocket object associated with a C4Socket
    websocket::WebSocket* WebSocketFrom(C4Socket *c4sock);


    /** WebSocket provider that uses the registered C4SocketFactory. */
    class C4Provider : public ProviderImpl {
    public:
        static C4Provider &instance();

        static void registerFactory(const C4SocketFactory&);

        virtual WebSocketImpl* createWebSocket(const Address &address,
                                               const fleeceapi::AllocedDict &options ={}) override;
        C4Socket* createWebSocket(const C4SocketFactory &factory,
                                  void *nativeHandle,
                                  const Address &address,
                                  const fleeceapi::AllocedDict &options ={});
    protected:
        virtual void openSocket(WebSocketImpl *s) override;
        virtual void requestClose(WebSocketImpl *s, int status, fleece::slice message) override;
        virtual void closeSocket(WebSocketImpl *s) override;
        virtual void sendBytes(WebSocketImpl *s, fleece::alloc_slice bytes) override;
        virtual void receiveComplete(WebSocketImpl *s, size_t byteCount) override;

    private:
        C4Provider() { }
        static void validateFactory(const C4SocketFactory&);
        static C4SocketFactory& registeredFactory();

        static C4SocketFactory *sRegisteredFactory;
    };


    /** A default C4SocketFactory, which will be registered when the first replication starts,
        if the app has not registered its own custom factory yet. */
    extern const C4SocketFactory C4DefaultSocketFactory;

} }
