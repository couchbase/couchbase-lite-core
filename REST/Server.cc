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
#include "Request.hh"
#include "Error.hh"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "c4ListenerInternal.hh"
#include "civetweb.h"

using namespace std;

namespace litecore { namespace REST {

    Server::Server(const char **options, void *owner)
    :_owner(owner)
    {
		static once_flag f;
		call_once(f, [=] {
			// Initialize the library (otherwise Windows crashes)
			mg_init_library(0);
		});

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
                rq.respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            else
                (handler(rq));
            rq.finish();
            return int(rq.status());
        } catch (const std::exception &x) {
            C4Warn("HTTP handler caught C++ exception: %s", x.what());
            mg_send_http_error(conn, 500, "Internal exception");
            return 500;
        }
    }

} }
