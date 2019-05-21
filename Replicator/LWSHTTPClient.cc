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
#include "LWSUtil.hh"
#include "Writer.hh"


namespace litecore { namespace REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::websocket;


    LWSHTTPClient::LWSHTTPClient(Response &response)
    :_response(response)
    { }


    void LWSHTTPClient::connect(const C4Address &address,
                                const char *method,
                                fleece::Doc headers,
                                alloc_slice requestBody)
    {
        _requestHeaders = headers;
        setDataToSend(requestBody);
        LWSContext::initialize();
        LWSContext::instance->connectClient(this,
                                            LWSContext::kHTTPClientProtocol,
                                            litecore::repl::Address(address),
                                            nullslice, method);
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
    void LWSHTTPClient::dispatch(lws *wsi, int reason, void *user, void *in, size_t len)
    {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
                LogDebug("**** LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER");
                onSendHeaders(in, len);
                break;
            case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
                LogDebug("**** LWS_CALLBACK_CLIENT_HTTP_WRITEABLE");
                onWriteRequest();
                break;
            case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
                LogDebug("**** LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP");
                onResponseAvailable();
                break;
            case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
                LogDebug("**** LWS_CALLBACK_RECEIVE_CLIENT_HTTP");
                onDataAvailable();
                break;
            case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
                onRead(slice(in, len));
                break;
            case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
            case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
                onCompleted(reason);
                break;

            default:
                LWSProtocol::dispatch(wsi, reason, user, in, len);
        }
    }


    void LWSHTTPClient::onConnectionError(C4Error error) {
        _error = error;
        notifyFinished();
    }


    bool LWSHTTPClient::onSendHeaders(void *in, size_t len) {
        auto dst = (uint8_t**)in;
        uint8_t *end = *dst + len;
        auto dict = _requestHeaders.root().asDict();
        for (Dict::iterator i(dict); i; ++i) {
            string name = string(i.keyString()) + ':';
            addRequestHeader(dst, end, name.c_str(), i.value().asString());
        }

        if (hasDataToSend()) {
            addContentLengthHeader(dst, end, dataToSend().size);
            lws_client_http_body_pending(_client, true);
            callbackOnWriteable();
        }
        return true;
    }


    bool LWSHTTPClient::onWriteRequest() {
        if (!sendMoreData(false))
            return false;
        if (hasDataToSend()) {
            callbackOnWriteable();
        } else {
            lws_client_http_body_pending(_client, false);
        }
        return true;
    }


    void LWSHTTPClient::onResponseAvailable() {
        int status;
        string message;
        tie(status, message) = decodeHTTPStatus();
        LogDebug("Got response: %d %s", status, message.c_str());
        _response.setStatus(status, message);
        _response.setHeaders(encodeHTTPHeaders());
    }


    void LWSHTTPClient::onDataAvailable() {
        char buffer[1024 + LWS_PRE];
        char *start = buffer + LWS_PRE;
        int len = sizeof(buffer) - LWS_PRE;
        // this will call back into the event loop with LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ...
        if (lws_http_client_read(_client, &start, &len) != 0)
            setDispatchResult(-1);
    }


    void LWSHTTPClient::onRead(slice data) {
        LogDebug("**** LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ: %zu bytes", data.size);
        _responseData.write(data);
    }


    void LWSHTTPClient::onCompleted(int reason) {
        if (!_finished) {
            _response.setBody(_responseData.finish());
            LogDebug("**** %-s: %zd-byte response body",
                     LWSCallbackName(reason), _response.body().size);
            setDispatchResult(-1); // close connection
            notifyFinished();
        }
    }


    const char * LWSHTTPClient::className() const noexcept      {return "LWSHTTPClient";}


} }
