//
//  Server.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Server.hh"
#include "Request.hh"
#include "c4Base.h"
#include "civetweb.h"

using namespace std;

namespace litecore { namespace REST {

    C4LogDomain RESTLog;

#if DEBUG
    // Also declared in civetweb.pch
    extern "C" void lc_civet_trace(const char *func, unsigned line, const char *fmt, ...);

    void lc_civet_trace(const char *func, unsigned line, const char *fmt, ...) {
        char *message;
        va_list args;
        va_start(args, fmt);
        vasprintf(&message, fmt, args);
        va_end(args);

        c4log(RESTLog, kC4LogDebug, "%s  (%s:%u)", message, func, line);
        free(message);
    }
#endif


    Server::Server(const char **options) {
        if (!RESTLog)
            RESTLog = c4log_getDomain("REST", true);

        mg_callbacks cb { };
        cb.log_message = [](const struct mg_connection *, const char *message) {
            c4log(RESTLog, kC4LogInfo, "%s", message);
            return -1; // disable default logging
        };
        cb.log_access = [](const struct mg_connection *, const char *message) {
            c4log(RESTLog, kC4LogInfo, "%s", message);
            return -1; // disable default logging
        };
        _context = mg_start(&cb, this, options);
        assert(_context);//FIX
    }

    Server::~Server() {
        if (_context)
            mg_stop(_context);
    }


    void Server::addHandler(Method method, const char *uri, const Handler &h) {
        string uriStr(uri);
        auto i = _handlers.find(uriStr);
        if (i == _handlers.end()) {
            URIHandlers handlers;
            handlers[method] = h;
            bool inserted;
            tie(i, inserted) = _handlers.insert({uriStr, handlers});
            mg_set_request_handler(_context, uri, &requestHandler, &i->second);
        }
        i->second[method] = h;
    }


    int Server::requestHandler(struct mg_connection *conn, void *cbdata) {
        const char *m = mg_get_request_info(conn)->request_method;
        Method method;
        if (strcmp(m, "GET") == 0)
            method = GET;
        else if (strcmp(m, "PUT") == 0)
            method = PUT;
        else if (strcmp(m, "DELETE") == 0)
        method = DELETE;
        else if (strcmp(m, "POST") == 0)
            method = POST;
        else
            return 0;
        
        auto handlers = (URIHandlers*)cbdata;
        Handler &handler = (*handlers)[method] ? (*handlers)[method] : (*handlers)[DEFAULT];
        Request rq(conn);
        if (!handler)
            rq.respondWithError(405, "Method not allowed");
        else
            (handler(rq));
        rq.finish();
        return rq.status();
    }

} }
