//
// LWSResponder.cc
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

#include "LWSResponder.hh"
#include "LWSServer.hh"
#include "LWSUtil.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "netUtils.hh"


namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace REST;


    static constexpr size_t kHeadersMaxSize = 10000;


    LWSResponder::LWSResponder(LWSServer *server, lws *connection)
    :LWSProtocol(connection)
    ,_server(server)
    {
        lws_set_opaque_user_data(connection, this);
        LogVerbose("Created %p on wsi %p", this, connection);
    }


    LWSResponder::~LWSResponder() {
        C4LogToAt(kC4WebSocketLog, kC4LogDebug, "~LWSResponder %p", this);
    }


    // Dispatch events sent by libwebsockets.
    void LWSResponder::dispatch(lws *wsi, int reason, void *user, void *in, size_t len)
    {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_HTTP:
                LogDebug("**** LWS_CALLBACK_HTTP");
                onURIReceived(slice(in, len));
                break;
            case LWS_CALLBACK_HTTP_BODY:
                LogDebug("**** LWS_CALLBACK_HTTP_BODY");
                onRequestBody(slice(in, len));
                break;
            case LWS_CALLBACK_HTTP_BODY_COMPLETION:
                LogDebug("**** LWS_CALLBACK_HTTP_BODY_COMPLETION");
                onRequestBodyComplete();
                break;
            case LWS_CALLBACK_HTTP_WRITEABLE:
                LogDebug("**** LWS_CALLBACK_HTTP_WRITEABLE");
                onWriteRequest();
                return;
            case LWS_CALLBACK_HTTP_CONFIRM_UPGRADE:
                LogDebug("**** LWS_CALLBACK_HTTP_CONFIRM_UPGRADE");
                if (!onWebSocketUpgrade({in, len}))
                    setDispatchResult(-1);      // TODO: Respond with error (and return 1)
            default:
                LWSProtocol::dispatch(wsi, reason, user, in, len);
        }
    }


    void LWSResponder::onRequestBody(slice body) {
        _requestBody.emplace_back(body);
    }


    void LWSResponder::onRequestBodyComplete() {
        // Concatenate all the chunks in the _requestBody vector:
        alloc_slice body;
        switch (_requestBody.size()) {
            case 0:
                return;
            case 1:
                body = _requestBody[0];
                break;
            default: {
                size_t size = 0;
                for (auto &chunk : _requestBody)
                    size += chunk.size;
                body = alloc_slice(size);
                auto dst = (uint8_t*)body.buf;
                for (auto &chunk : _requestBody) {
                    memcpy(dst, chunk.buf, chunk.size);
                    dst += chunk.size;
                }
            }
        }
        _requestBody.clear();
        LogVerbose("Received %zu-byte request body", body.size);
        onRequestBody(body);
        onRequestComplete();
    }


    void LWSResponder::onURIReceived(slice uri) {
        _responseHeaders = alloc_slice(kHeadersMaxSize);
        _responseHeadersPos = (uint8_t*)_responseHeaders.buf;

        auto method = getMethod();
        onRequest(method,
                  string("/") + string(uri),
                  getHeader(WSI_TOKEN_HTTP_URI_ARGS),
                  encodeHTTPHeaders());

        auto contentLength = getContentLengthHeader();
        if (contentLength == 0 || (contentLength < 0 && method == Method::GET))
            onRequestComplete();
    }


    void LWSResponder::onRequestComplete() {
        _server->dispatchRequest(this);
        finish();
        _server = nullptr;
    }


    void LWSResponder::onConnectionError(C4Error error) {
        _error = error;
    }


    Method LWSResponder::getMethod() {
        if (hasHeader(WSI_TOKEN_GET_URI))           return Method::GET;
        else if (hasHeader(WSI_TOKEN_PUT_URI))      return Method::PUT;
        else if (hasHeader(WSI_TOKEN_DELETE_URI))   return Method::DELETE;
        else if (hasHeader(WSI_TOKEN_POST_URI))     return Method::POST;
        else if (hasHeader(WSI_TOKEN_OPTIONS_URI))  return Method::OPTIONS;
        else                                        return Method::None;
    }


#pragma mark - RESPONSE STATUS LINE:


    void LWSResponder::setStatus(HTTPStatus status, const char *message) {
        Assert(!_sentStatus);
        _status = status;
        _statusMessage = message ? message : "";
        sendStatus();
    }


    void LWSResponder::sendStatus() {
        if (_sentStatus)
            return;
        Log("Response status: %d", _status);
        //FIXME: How to include the message?
        check(lws_add_http_header_status(_client, unsigned(_status),
                                         &_responseHeadersPos,
                                         (uint8_t*)_responseHeaders.end()));
        _sentStatus = true;

        // Now add the Date header:
        char date[50];
        time_t curtime = time(NULL);
        gmt_time_string(date, sizeof(date), &curtime);
        setHeader("Date", date);
    }


    void LWSResponder::writeStatusJSON(HTTPStatus status, const char *message) {
        auto &json = jsonEncoder();
        if (int(status) < 300) {
            json.writeKey("ok"_sl);
            json.writeBool(true);
        } else {
            json.writeKey("status"_sl);
            json.writeInt(int(status));
            const char *defaultMessage = StatusMessage(status);
            if (defaultMessage) {
                json.writeKey("error"_sl);
                json.writeString(defaultMessage);
            }
            if (message && defaultMessage && 0 != strcasecmp(message, defaultMessage)) {
                json.writeKey("reason"_sl);
                json.writeString(message);
            }
        }
    }


    void LWSResponder::writeErrorJSON(C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        writeStatusJSON(errorToStatus(err),
                        (message ? message.asString().c_str() : nullptr));
    }


    void LWSResponder::respondWithStatus(HTTPStatus status, const char *message) {
        setStatus(status, message);
        uncacheable();

        if (status >= HTTPStatus::OK && status != HTTPStatus::NoContent
                                     && status != HTTPStatus::NotModified) {
            auto &json = jsonEncoder();
            json.beginDict();
            writeStatusJSON(status, message);
            json.endDict();
        }
    }


    void LWSResponder::respondWithError(C4Error err) {
        Assert(err.code != 0);
        alloc_slice message = c4error_getMessage(err);
        respondWithStatus(errorToStatus(err),
                          (message ? message.asString().c_str() : nullptr));
    }


    HTTPStatus LWSResponder::errorToStatus(C4Error err) {
        if (err.code == 0)
            return HTTPStatus::OK;
        HTTPStatus status = HTTPStatus::ServerError;
        // TODO: Add more mappings, and make these table-driven
        switch (err.domain) {
            case LiteCoreDomain:
                switch (err.code) {
                    case kC4ErrorInvalidParameter:
                    case kC4ErrorBadRevisionID:
                        status = HTTPStatus::BadRequest; break;
                    case kC4ErrorNotADatabaseFile:
                    case kC4ErrorCrypto:
                        status = HTTPStatus::Unauthorized; break;
                    case kC4ErrorNotWriteable:
                        status = HTTPStatus::Forbidden; break;
                    case kC4ErrorNotFound:
                        status = HTTPStatus::NotFound; break;
                    case kC4ErrorConflict:
                        status = HTTPStatus::Conflict; break;
                    case kC4ErrorUnimplemented:
                    case kC4ErrorUnsupported:
                        status = HTTPStatus::NotImplemented; break;
                    case kC4ErrorRemoteError:
                        status = HTTPStatus::GatewayError; break;
                    case kC4ErrorBusy:
                        status = HTTPStatus::Locked; break;
                }
                break;
            case WebSocketDomain:
                if (err.code < 1000)
                    status = HTTPStatus(err.code);
            default:
                break;
        }
        return status;
    }


#pragma mark - RESPONSE HEADERS:


    void LWSResponder::setHeader(const char *header, const char *value) {
        sendStatus();
        string headerWithColon = string(header) + ':';
        Assert(_responseHeaders);
        check(lws_add_http_header_by_name(_client, (const uint8_t*)headerWithColon.c_str(),
                                          (const uint8_t*)value, (int)strlen(value),
                                          &_responseHeadersPos,
                                          (uint8_t*)_responseHeaders.end()));
    }


    void LWSResponder::addHeaders(map<string, string> headers) {
        for (auto &entry : headers)
            setHeader(entry.first.c_str(), entry.second.c_str());
    }


    void LWSResponder::setContentLength(uint64_t length) {
        sendStatus();
        Assert(_contentLength < 0, "Content-Length has already been set");
        Log("Content-Length: %llu", length);
        _contentLength = (int64_t)length;
        check(lws_add_http_header_content_length(_client, length,
                                                    &_responseHeadersPos,
                                                    (uint8_t*)_responseHeaders.end()));
    }


    void LWSResponder::sendHeaders() {
        if (_jsonEncoder)
            setHeader("Content-Type", "application/json");

        check(lws_finalize_write_http_header(_client,
                                             (uint8_t*)_responseHeaders.buf,
                                             &_responseHeadersPos,
                                             (uint8_t*)_responseHeaders.end()));
        _responseHeaders = nullslice;
        _responseHeadersPos = nullptr;
    }


#pragma mark - RESPONSE BODY:


    void LWSResponder::uncacheable() {
        setHeader("Cache-Control", "no-cache, no-store, must-revalidate, private, max-age=0");
        setHeader("Pragma", "no-cache");
        setHeader("Expires", "0");
    }


    void LWSResponder::write(slice content) {
        Assert(!_finished);
        LogDebug("Write: `%.*s`", SPLAT(content));
        _responseWriter.write(content);
    }


    void LWSResponder::printf(const char *format, ...) {
        char *str;
        va_list args;
        va_start(args, format);
        size_t length = vasprintf(&str, format, args);
        va_end(args);
        write({str, length});
        free(str);
    }


    fleece::JSONEncoder& LWSResponder::jsonEncoder() {
        if (!_jsonEncoder)
            _jsonEncoder.reset(new fleece::JSONEncoder);
        return *_jsonEncoder;
    }


    void LWSResponder::finish() {
        if (_finished)
            return;

        if (_jsonEncoder) {
            alloc_slice json = _jsonEncoder->finish();
            write(json);
        }

        alloc_slice responseData = _responseWriter.finish();
        if (_contentLength < 0)
            setContentLength(responseData.size);
        else
            Assert(_contentLength == responseData.size);

        sendHeaders();

        Log("Now sending body...");
        setDataToSend(responseData);
        _finished = true;
    }


    // Handles LWS_CALLBACK_HTTP_WRITEABLE
    void LWSResponder::onWriteRequest() {
        sendMoreData(true);
        if (!hasDataToSend()) {
            if (lws_http_transaction_completed(_client))
                setDispatchResult(1);       // to close connection
        }
    }

} }
