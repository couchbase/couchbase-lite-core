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
#include "libwebsockets.h"
#include "LWSContext.hh"

using namespace std;

namespace litecore { namespace REST {
    using namespace litecore::websocket;


    Server::Server(uint16_t port, const char *hostname, void *owner)
    :_owner(owner)
    {
        lws_http_mount mount = {};
        mount.mountpoint = "/";
        mount.mountpoint_len = 1;
        mount.protocol = LWSContext::kHTTPServerProtocol;
        mount.origin_protocol = LWSMPRO_CALLBACK;

        LWSContext::initialize();
        _vhost = LWSContext::instance->startServer(this, port, hostname, &mount);
        if (!_vhost)
            error::_throw(error::UnexpectedError, "Couldn't start civetweb server");
    }

    Server::~Server() {
        if (_vhost)
            lws_vhost_destroy(_vhost);      //??? Is this thread-safe?
    }


    int Server::dispatch(lws *client, int reason, void *user, void *in, size_t len) {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_HTTP:
                return onRequest(slice(in, len)) ? 0 : -1;
            default:
                return LWSProtocol::dispatch(client, reason, user, in, len);
        }
    }


    void Server::onConnectionError(C4Error error) {
        // TODO ???
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
        }
        i->second.methods[method] = h;
    }


    bool Server::onRequest(slice uri) {
        try {
#if 1
            uint8_t buf[2048], *start = &buf[0], *p = start, *end = &buf[sizeof(buf) - 1];
            if (0 != lws_add_http_common_headers(_client, 20, "text/plain", LWS_ILLEGAL_HTTP_CONTENT_LEN, &p, end))
                return false;
            if (lws_finalize_write_http_header(_client, start, &p, end))
                return 1;
            lws_callback_on_writable(_client);
            return true;
#else
            Method method = GET;    // FIX

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

            RequestResponse rq(method, uri, encodeHTTPHeaders());
            rq.addHeaders(extraHeaders);
            if (!handler)
                rq.respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            else
                (handler(rq));
            rq.finish();
            return int(rq.status());
#endif
        } catch (const std::exception &x) {
            C4Warn("HTTP handler caught C++ exception: %s", x.what());
            return 0 == lws_return_http_status(_client, 500, "Internal exception");
        }
    }

} }
