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

struct mg_context;
struct mg_connection;

namespace litecore { namespace REST {
    class Request;

    /** HTTP server, using CivetWeb. */
    class Server {
    public:
        Server(const char **options);

        ~Server();

        enum Method {
            DEFAULT,
            GET,
            PUT,
            DELETE,
            POST,
        };

        using Handler = std::function<void(Request&)>;

        void addHandler(Method, const char *uri, const Handler &h);

    private:
        static int requestHandler(mg_connection *conn, void *cbdata);

        using URIHandlers = std::array<Handler, 4>;
        
        mg_context* _context;
        std::map<std::string, URIHandlers> _handlers;
    };

} }
