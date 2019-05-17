//
// LWSServer.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "LWSServer.hh"
#include "LWSResponder.hh"
#include "LWSContext.hh"
#include "LWSUtil.hh"
#include "Error.hh"
#include "libwebsockets.h"


#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "LWSServer: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "LWSServer: " MSG, ##__VA_ARGS__)


namespace litecore { namespace websocket {

    LWSServer::LWSServer(uint16_t port, const char *hostname)
    :_mount(new lws_http_mount)
    {
        memset(_mount.get(), 0, sizeof(*_mount));
        _mount->mountpoint = "/";
        _mount->mountpoint_len = 1;
        _mount->protocol = LWSContext::kHTTPServerProtocol;
        _mount->origin_protocol = LWSMPRO_CALLBACK;

        LWSContext::initialize();
        _vhost = LWSContext::instance->startServer(this, port, hostname, _mount.get());
        if (!_vhost)
            error::_throw(error::UnexpectedError, "Couldn't start LWSServer");
    }


    LWSServer::~LWSServer() {
        if (_vhost)
            lws_vhost_destroy(_vhost);      //??? Is this thread-safe?
    }


    int LWSServer::dispatch(lws *client, int reason, void *user, void *in, size_t len) {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
                return createResponder(client) ? 0 : -1;
            default:
                if (reason < 31 || reason > 36)
                    LogDebug("**** %s", LWSCallbackName(reason));
                return lws_callback_http_dummy(client, (lws_callback_reasons)reason, user, in, len);
        }
    }


    bool LWSServer::createResponder(lws *client) {
        (void) new REST::LWSResponder(this, client);
        return true;
    }


} }
