//
// CivetWebSocket.hh
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
#include "Fleece.h"
#include "c4Socket.h"
#include <functional>
#include <set>

struct mg_connection;
struct mg_context;

namespace litecore { namespace websocket {

    /** A WebSocket provider based on the CivetWeb HTTP library. */
    class CivetProvider : public Provider {
    public:

        static CivetProvider& instance();
        
        /** Creates a client WebSocket to a given address. */
        virtual WebSocket* createWebSocket(const Address&,
                                           const fleeceapi::AllocedDict &options ={}) override;

        /** Callback when an HTTP server receives a WebSocket connection. */
        using ServerWebSocketHandler = std::function<bool(const mg_connection*, WebSocket*)>;

        /** Registers a WebSocket handler for an HTTP server. */
        void setServerWebSocketHandler(mg_context*, std::string uri, ServerWebSocketHandler);

        static C4SocketFactory C4SocketFactory();

    private:
        static int connectHandler(const struct mg_connection *roConnection, void *context);
};

} }
