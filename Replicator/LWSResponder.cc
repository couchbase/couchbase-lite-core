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
#include "Error.hh"
#include "civetUtils.hh"
#include "libwebsockets.h"

#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "LWSResponder: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "LWSResponder: " MSG, ##__VA_ARGS__)


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
        C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "Created LWSResponder on wsi %p", connection);
    }


    // Dispatch events sent by libwebsockets.
    int LWSResponder::dispatch(lws *wsi, int reason, void *user, void *in, size_t len)
    {
        switch ((lws_callback_reasons)reason) {
            case LWS_CALLBACK_HTTP:
                return onRequestReady(slice(in, len)) ? 0 : -1;
            case LWS_CALLBACK_HTTP_WRITEABLE:
                return onWriteRequest() ? 0 : -1;
            default:
                return LWSProtocol::dispatch(wsi, reason, user, in, len);
        }
    }


    bool LWSResponder::onRequestReady(slice uri) {
        setRequest(getMethod(), alloc_slice(uri), nullslice, encodeHTTPHeaders(), {});

        _responseHeaders = alloc_slice(kHeadersMaxSize);
        _responseHeadersPos = (uint8_t*)_responseHeaders.buf;

        char date[50];
        time_t curtime = time(NULL);
        gmt_time_string(date, sizeof(date), &curtime);
        setHeader("Date", date);

        _server->dispatchResponder(this);
        finish();
        _server = nullptr;
        return true;
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
        else
            return Method::DEFAULT;
    }

    void LWSResponder::setStatus(HTTPStatus status, const char *message) {
        Assert(_responseHeaders);
        Assert(!_sentStatus);
        _status = status;
        _statusMessage = message;

        //FIXME: How to include the message?
        if (0 != lws_add_http_header_status(_client, unsigned(_status),
                                             &_responseHeadersPos,
                                             (uint8_t*)_responseHeaders.end())) {
            abort(); //FIXME
        }

        _sentStatus = true;
    }


    void LWSResponder::writeStatusJSON(HTTPStatus status, const char *message) {
        auto &json = jsonEncoder();
        if (int(status) < 300) {
            json.writeKey("ok"_sl);
            json.writeBool(true);
        } else {
            //const char *defaultMessage = mg_get_response_code_text(_conn, int(status));
            json.writeKey("status"_sl);
            json.writeInt(int(status));
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


    void LWSResponder::setHeader(const char *header, const char *value) {
        Assert(_responseHeaders);
        if (0 != lws_add_http_header_by_name(_client, (const uint8_t*)header,
                                             (const uint8_t*)value, (int)strlen(value),
                                             &_responseHeadersPos,
                                             (uint8_t*)_responseHeaders.end())) {
            abort(); //FIXME
        }
    }


    void LWSResponder::addHeaders(map<string, string> headers) {
        for (auto &entry : headers)
            setHeader(entry.first.c_str(), entry.second.c_str());
    }


    void LWSResponder::setContentLength(uint64_t length) {
        Assert(!_chunked);
        Assert(_contentLength < 0);
        _contentLength = (int64_t)length;
        if (0 != lws_add_http_header_content_length(_client, length,
                                                    &_responseHeadersPos,
                                                    (uint8_t*)_responseHeaders.end()))
            abort(); //FIXME
    }


    bool LWSResponder::sendHeaders() {
        int err = lws_finalize_write_http_header(_client,
                                                 (uint8_t*)_responseHeaders.buf,
                                                 &_responseHeadersPos,
                                                 (uint8_t*)_responseHeaders.end());
        _responseHeaders = nullslice;
        _responseHeadersPos = nullptr;
        return err == 0;
    }


#pragma mark - RESPONSE BODY:


    void LWSResponder::setChunked() {
        if (!_chunked) {
            Assert(_contentLength < 0);
            _chunked = true;
        }
    }


    void LWSResponder::uncacheable() {
        setHeader("Cache-Control", "no-cache, no-store, must-revalidate, private, max-age=0");
        setHeader("Pragma", "no-cache");
        setHeader("Expires", "0");
    }


    void LWSResponder::write(slice content) {
        if (_responseHeaders) {
            if (!_chunked)
                setContentLength(content.size);
            sendHeaders();
        }
        if (!_chunked)
            Assert(_contentLength >= 0);
        _responseData.write(content);
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
        setHeader("Content-Type", "application/json");
        if (!_jsonEncoder)
            _jsonEncoder.reset(new fleece::JSONEncoder);
        return *_jsonEncoder;
    }


    void LWSResponder::finish() {
        if (_jsonEncoder) {
            alloc_slice json = _jsonEncoder->finish();
            write(json);
        }
        if (_contentLength < 0 && !_chunked)
            setContentLength(0);
        sendHeaders();
        if (!_chunked)
            Assert(_contentLength == _contentSent);

        setDataToSend(_responseData.finish());
        if (hasDataToSend())
            lws_callback_on_writable(_client);
    }


    bool LWSResponder::onWriteRequest() {
        if (!sendMoreData())
            return false;
        if (hasDataToSend()) {
            lws_callback_on_writable(_client);
            return true;
        } else {
            return 0 == lws_http_transaction_completed(_client);
        }
    }


} }
