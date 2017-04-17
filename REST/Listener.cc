//
//  Listener.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//
// <https://github.com/civetweb/civetweb/blob/master/docs/UserManual.md#configuration-options>

#include "Listener.hh"
#include "c4REST.h"
#include "Server.hh"
#include "Request.hh"
#include "StringUtil.hh"
#include <functional>
#include <queue>

using namespace std;
using namespace fleece;


C4RESTListener* c4rest_start(uint16_t port, C4Error *error) {
    return (C4RESTListener*) new litecore::REST::Listener(port);
}

void c4rest_free(C4RESTListener *listener) {
    delete (litecore::REST::Listener*)listener;
}


namespace litecore { namespace REST {

#define kKeepAliveTimeoutMS     "1000"
#define kMaxConnections         "8"


    Listener::Listener(uint16_t port) {
        auto portStr = to_string(port);
        const char* options[] {
            "listening_ports",          portStr.c_str(),
            "enable_keep_alive",        "yes",
            "keep_alive_timeout_ms",    kKeepAliveTimeoutMS,
            "num_threads",              kMaxConnections,
            nullptr
        };
        _server.reset(new Server(options));

        auto notFound =  [](Request &rq) { rq.respondWithError(404, "Not Found"); };

        // Root:
        _server->addHandler(Server::GET, "/$", [](Request &rq) {
            rq.setHeader("Content-Type", "text/plain; charset=utf-8");
            rq.write("Hello world!");
        });

        // Top-level special handlers:
        _server->addHandler(Server::GET, "/_all_dbs$", [](Request &rq) {
            rq.setHeader("Content-Type", "text/plain; charset=utf-8");
            rq.write("All DBs");
        });

        _server->addHandler(Server::DEFAULT, "/_", notFound);

        // Database:
        _server->addHandler(Server::GET, "/*$|/*/$", [](Request &rq) {
            slice dbName = rq.path(0);
            rq.setHeader("Content-Type", "text/plain; charset=utf-8");
            rq.setChunked();
            rq.printf("Database \"%.*s\"\n", SPLAT(dbName));
        });

        // Database-level special handlers:
        _server->addHandler(Server::GET, "/*/_all_docs$", [](Request &rq) {
            rq.setHeader("Content-Type", "text/plain; charset=utf-8");
            slice db = rq.path(0);
            rq.printf("All docs in %.*s", SPLAT(db));
        });

        _server->addHandler(Server::DEFAULT, "/*/_", notFound);

        // Document:
        _server->addHandler(Server::GET, "/*/*$", [](Request &rq) {
            slice dbName = rq.path(0);
            slice docID = rq.path(1);
            rq.setHeader("Content-Type", "text/plain; charset=utf-8");
            rq.printf("Database \"%.*s\", doc \"%.*s\"\n", SPLAT(dbName), SPLAT(docID));
        });
    }


    Listener::~Listener() {
    }


    void Listener::registerDatabase(string name, C4Database *db) {
        _databases[name] = c4db_retain(db);
    }

} }
