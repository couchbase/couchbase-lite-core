//
//  Server.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <array>
#include <map>
#include <mutex>

struct mg_context;
struct mg_connection;

namespace litecore { namespace REST {
    class Request;

    /** HTTP server, using CivetWeb. */
    class Server {
    public:
        Server(const char **options, void *owner =nullptr);

        ~Server();

        void* owner() const                         {return _owner;}

        enum Method {
            DEFAULT,
            GET,
            PUT,
            DELETE,
            POST,

            kNumMethods
        };

        using Handler = std::function<void(Request&)>;

        void addHandler(Method, const char *uri, const Handler &h);

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
    };

} }
