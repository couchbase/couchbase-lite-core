//
//  CivetWebSocket.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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
        
        virtual void addProtocol(const std::string &protocol) override;

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

        std::set<std::string> _protocols;
};

} }
