//
//  Server.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Server.hh"
#include "Request.hh"
#include "Logging.hh"
#include "Error.hh"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "civetweb.h"

using namespace std;

namespace litecore { namespace REST {

    C4LogDomain RESTLog;


    Server::Server(const char **options, void *owner)
    :_owner(owner)
    {
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
        if (!_context)
            error::_throw(error::UnexpectedError, "Couldn't start civetweb server");
    }

    Server::~Server() {
        if (_context)
            mg_stop(_context);
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
            handlers.methods[method] = h;
            bool inserted;
            tie(i, inserted) = _handlers.insert({uriStr, handlers});
            mg_set_request_handler(_context, uri, &handleRequest, &i->second);
        }
        i->second.methods[method] = h;
    }


    int Server::handleRequest(struct mg_connection *conn, void *cbdata) {
        try {
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
            Handler handler;
            map<string, string> extraHeaders;
            {
                lock_guard<mutex> lock(handlers->server->_mutex);
                handler = handlers->methods[method];
                if (!handler)
                    handler = handlers->methods[DEFAULT];
                extraHeaders = handlers->server->_extraHeaders;
            }

            RequestResponse rq(conn);
            rq.addHeaders(extraHeaders);
            if (!handler)
                rq.respondWithError(HTTPStatus::MethodNotAllowed, "Method not allowed");
            else
                (handler(rq));
            rq.finish();
            return int(rq.status());
        } catch (const std::exception &x) {
            Warn("HTTP handler caught C++ exception: %s", x.what());
            mg_send_http_error(conn, 500, "Internal exception");
            return 500;
        }
    }

} }
