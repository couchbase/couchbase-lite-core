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
#include "civetUtils.hh"


namespace litecore { namespace REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::websocket;


    static constexpr size_t kHeadersMaxSize = 10000;


    LWSResponder::LWSResponder(LWSServer *server, lws *connection)
    :LWSProtocol(connection)
    ,_server(server)
    {
        lws_set_opaque_user_data(connection, this);
        LogVerbose("Created LWSResponder on wsi %p", connection);
    }


    // Dispatch events sent by libwebsockets.
    void LWSResponder::dispatch(lws *wsi, int reason, void *user, void *in, size_t len)
    {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_HTTP:
                onRequestReady(slice(in, len));
                break;
            case LWS_CALLBACK_HTTP_WRITEABLE:
                onWriteRequest();
                return;
            default:
                LWSProtocol::dispatch(wsi, reason, user, in, len);
        }
    }


    void LWSResponder::onRequestReady(slice uri) {
        setRequest(getMethod(), string("/") + string(uri), nullslice, encodeHTTPHeaders(), {});

        _responseHeaders = alloc_slice(kHeadersMaxSize);
        _responseHeadersPos = (uint8_t*)_responseHeaders.buf;

        _server->dispatchResponder(this);
        finish();
        _server = nullptr;
    }


    void LWSResponder::onConnectionError(C4Error error) {
        _error = error;
    }


    Method LWSResponder::getMethod() {
        if (hasHeader(WSI_TOKEN_GET_URI))
            return Method::GET;
        else if (hasHeader(WSI_TOKEN_PUT_URI))
            return Method::PUT;
        else if (hasHeader(WSI_TOKEN_DELETE_URI))
            return Method::DELETE;
        else if (hasHeader(WSI_TOKEN_POST_URI))
            return Method::POST;
        else if (hasHeader(WSI_TOKEN_OPTIONS_URI))
            return Method::OPTIONS;
        else
            return Method::None;
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
            //const char *defaultMessage = mg_get_response_code_text(_conn, int(status));
            //json.writeKey("error"_sl);
            //json.writeString(defaultMessage);
            if (message /*&& 0 != strcasecmp(message, defaultMessage)*/) {
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
        Log("Write: `%.*s`", SPLAT(content));
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
        if (!_jsonEncoder) {
            setHeader("Content-Type", "application/json");
            _jsonEncoder.reset(new fleece::JSONEncoder);
        }
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
        if (!hasDataToSend())
            check(lws_http_transaction_completed(_client));
    }

} }
