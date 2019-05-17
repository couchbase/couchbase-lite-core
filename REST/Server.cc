//
// Server.cc
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

#include "Server.hh"
#include "LWSResponder.hh"
#include "Request.hh"
#include "Error.hh"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "c4ListenerInternal.hh"
#include "libwebsockets.h"
#include "LWSContext.hh"

using namespace std;

#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "Server: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "Server: " MSG, ##__VA_ARGS__)


namespace litecore { namespace REST {
    using namespace litecore::websocket;


    Server::Server(uint16_t port, const char *hostname, void *owner)
    :LWSServer(port, hostname)
    ,_owner(owner)
    { }

    Server::~Server() {
    }


    void Server::setExtraHeaders(const std::map<std::string, std::string> &headers) {
        lock_guard<mutex> lock(_mutex);
        _extraHeaders = headers;
    }


    void Server::addHandler(Method method, const char *uri, const Handler &h) {
        lock_guard<mutex> lock(_mutex);

        string uriStr(uri);
        auto i = _handlers.find(uriStr);
        if (i == _handlers.end()) {
            URIHandlers handlers;
            handlers.server = this;
            handlers.methods[unsigned(method)] = h;
            bool inserted;
            tie(i, inserted) = _handlers.insert({uriStr, handlers});
        }
        i->second.methods[unsigned(method)] = h;
    }


    void Server::dispatchResponder(LWSResponder *rq) {
        try{
            Handler handler;
            map<string, string> extraHeaders;
            {
                lock_guard<mutex> lock(handlers->server->_mutex);
                handler = handlers->methods[method];
                if (!handler)
                    handler = handlers->methods[DEFAULT];
                extraHeaders = handlers->server->_extraHeaders;
            }

            if (!handler)
                rq->respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            else
                (handler(*rq));
            rq->finish();
        } catch (const std::exception &x) {
            C4Warn("HTTP handler caught C++ exception: %s", x.what());
            rq->respondWithStatus(HTTPStatus::ServerError, "Internal exception");
        }
    }

} }
