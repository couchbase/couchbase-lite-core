//
// Server.hh
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
#include "LWSProtocol.hh"
#include "Request.hh"
#include "c4Base.h"
#include <array>
#include <map>
#include <mutex>
#include <functional>

struct lws_vhost;

namespace litecore { namespace REST {
    class RequestResponse;
    class Request;


    /** HTTP server, using CivetWeb. */
    class Server : public websocket::LWSProtocol {
    public:
        Server(uint16_t port,
               const char *hostname,
               void *owner =nullptr);

        ~Server();

        void* owner() const                         {return _owner;}

        void setExtraHeaders(const std::map<std::string, std::string> &headers);

        using Handler = std::function<void(RequestResponse&)>;

        void addHandler(Method, const char *uri, const Handler &h);

    protected:
        virtual int dispatch(lws*, int callback_reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;
        bool onRequest(fleece::slice uri);

    private:
        struct URIHandlers {
            Server* server;
            std::array<Handler, size_t(Method::kNumMethods)> methods;
        };

        void* const _owner;
        std::mutex _mutex;
        lws_vhost* _vhost {nullptr};
        std::map<std::string, URIHandlers> _handlers;
        std::map<std::string, std::string> _extraHeaders;
    };

} }
