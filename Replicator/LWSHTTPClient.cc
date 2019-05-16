//
// LWSHTTPClient.cc
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

#include "LWSHTTPClient.hh"
#include "LWSContext.hh"
#include "LWSProtocol.hh"
#include "WebSocketInterface.hh"
#include "Writer.hh"
#include "libwebsockets.h"


#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "LWSHTTPClient: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "LWSHTTPClient: " MSG, ##__VA_ARGS__)


namespace litecore { namespace REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::websocket;


    static constexpr size_t kWriteChunkSize = 1024;


    LWSHTTPClient::LWSHTTPClient(Response &response,
                                 const C4Address &address,
                                 const char *method,
                                 alloc_slice requestBody)
    :_response(response)
    ,_requestBody(requestBody)
    ,_unsentBody(requestBody)
    {
        LWSContext::initialize();
        auto client = LWSContext::instance->connectClient(this,
                                                          LWSContext::kHTTPClientProtocol,
                                                          litecore::repl::Address(address),
                                                          nullslice, method);
        if (!client) {
            _error = c4error_make(LiteCoreDomain, kC4ErrorUnexpectedError,
                                  "Could not open libwebsockets connection"_sl);
            _finished = true;
        }
    }


    C4Error LWSHTTPClient::run() {
        std::unique_lock<std::mutex> lock(_mutex);
        _condition.wait(lock, [&]() {return _finished;});
        return _error;
    }


    void LWSHTTPClient::notifyFinished() {
        std::unique_lock<std::mutex> lock(_mutex);
        _finished = true;
        _condition.notify_all();
    }


    // Dispatch events sent by libwebsockets.
    int LWSHTTPClient::dispatch(lws *wsi, int reason, void *user, void *in, size_t len)
    {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
                LogDebug("**** LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER");
                return onSendHeaders() ? 0 : -1;
            case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
                LogDebug("**** LWS_CALLBACK_CLIENT_HTTP_WRITEABLE");
                return onWriteRequest();
            case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
                LogDebug("**** LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP");
                onResponseAvailable();
                return 0;
            case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
                LogDebug("**** LWS_CALLBACK_RECEIVE_CLIENT_HTTP");
                return onDataAvailable() ? 0 : -1;
            case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
                onRead(slice(in, len));
                return 0;
            case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
                onCompleted();
                return 0;

            default:
                return LWSProtocol::dispatch(wsi, reason, user, in, len);
        }
    }


    void LWSHTTPClient::onConnectionError(C4Error error) {
        _error = error;
        notifyFinished();
    }


    bool LWSHTTPClient::onSendHeaders() {
        //TODO: Write the headers!
        if (_requestBody) {
            lws_client_http_body_pending(_client, 1);
            lws_callback_on_writable(_client);
        }
        return true;
    }


    bool LWSHTTPClient::onWriteRequest() {
        slice chunk = _unsentBody.read(kWriteChunkSize);
        lws_write_protocol type;
        if (_unsentBody.size > 0) {
            type = LWS_WRITE_HTTP;
            lws_callback_on_writable(_client);
        } else {
            type = LWS_WRITE_HTTP_FINAL;
            lws_client_http_body_pending(_client, 0);
            _requestBody = nullslice;
        }
        LogDebug("Writing %zu bytes", chunk.size);
        return 0 ==  lws_write(_client, (uint8_t*)chunk.buf, chunk.size, type);
    }


    void LWSHTTPClient::onResponseAvailable() {
        int status;
        string message;
        tie(status, message) = decodeHTTPStatus();
        LogDebug("Got response: %d %s", status, message.c_str());
        _response.setStatus(status, message);
        _response.setHeaders(encodeHTTPHeaders());
    }


    bool LWSHTTPClient::onDataAvailable() {
        char buffer[1024 + LWS_PRE];
        char *start = buffer + LWS_PRE;
        int len = sizeof(buffer) - LWS_PRE;
        // this will call back into the event loop with LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ...
        return lws_http_client_read(_client, &start, &len) == 0;
    }


    void LWSHTTPClient::onRead(slice data) {
        LogDebug("**** LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ: %zu bytes", data.size);
        _responseData.write(data);
    }


    void LWSHTTPClient::onCompleted() {
        _response.setBody(_responseData.finish());
        LogDebug("**** LWS_CALLBACK_COMPLETED_CLIENT_HTTP: %zd-byte response body",
                 _response.body().size);
        notifyFinished();
    }

} }
