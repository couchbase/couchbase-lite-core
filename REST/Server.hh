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
#include "c4Base.h"
#include <array>
#include <map>
#include <mutex>

struct mg_context;
struct mg_connection;

namespace litecore { namespace REST {
    class RequestResponse;
    class Request;


    /** HTTP server, using CivetWeb. */
    class Server {
    public:
        Server(const char **options, void *owner =nullptr);

        ~Server();

        void* owner() const                         {return _owner;}

        void setExtraHeaders(const std::map<std::string, std::string> &headers);

        enum Method {
            DEFAULT,
            GET,
            PUT,
            DELETE,
            POST,

            kNumMethods
        };

        using Handler = std::function<void(RequestResponse&)>;

        void addHandler(Method, const char *uri, const Handler &h);

        mg_context* mgContext() const               {return _context;}

    private:
        static int handleRequest(mg_connection *conn, void *cbdata);

        struct URIHandlers {
            Server* server;
            std::array<Handler, kNumMethods> methods;
        };

        void* const _owner;
        std::mutex _mutex;
        mg_context* _context;
        std::map<std::string, URIHandlers> _handlers;
        std::map<std::string, std::string> _extraHeaders;
    };

} }
