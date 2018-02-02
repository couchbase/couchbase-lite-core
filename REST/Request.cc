//
// Request.cc
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

#include "Request.hh"
#include "Writer.hh"
#include "civetUtils.hh"
#include "civetweb.h"
#include "PlatformIO.hh"
#include "Error.hh"
#include <stdarg.h>

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace REST {

#pragma mark - REQUEST:


    slice Request::method() const {
        return slice(mg_get_request_info(_conn)->request_method);
    }


    slice Request::path() const {
        return slice(mg_get_request_info(_conn)->request_uri);
    }

    
    string Request::path(int i) const {
        slice path(mg_get_request_info(_conn)->request_uri);
        Assert(path[0] == '/');
        path.moveStart(1);
        for (; i > 0; --i) {
            auto slash = path.findByteOrEnd('/');
            if (slash == path.end())
                return "";
            path.setStart(slash + 1);
        }
        auto slash = path.findByteOrEnd('/');
        if (slash == path.buf)
            return "";
        auto component = slice(path.buf, slash).asString();
        return urlDecode(component);
    }

    
    std::string Request::query(const char *param) const {
        std::string result;
        auto query = mg_get_request_info(_conn)->query_string;
        if (!query)
            return string();
        litecore::REST::getParam(query, strlen(query), param, result, 0);
        return result;
    }

    int64_t Request::intQuery(const char *param, int64_t defaultValue) const {
        string val = query(param);
        if (!val.empty()) {
            try {
                return stoll(val);
            } catch (...) { }
        }
        return defaultValue;
    }

    bool Request::boolQuery(const char *param, bool defaultValue) const {
        string val = query(param);
        if (val.empty())
            return defaultValue;
        return val != "false" && val != "0";        // same behavior as Obj-C CBL 1.x
    }


#pragma mark - REQUESTRESPONSE:


    RequestResponse::RequestResponse(mg_connection *conn)
    :Request(conn)
    {
        char date[50];
        time_t curtime = time(NULL);
        gmt_time_string(date, sizeof(date), &curtime);
        setHeader("Date", date);
    }


    void RequestResponse::setStatus(HTTPStatus status, const char *message) {
        Assert(!_sentStatus);
        if (!message)
            message = mg_get_response_code_text(_conn, int(status));
        mg_printf(_conn, "HTTP/1.1 %d %s\r\n", status, message);
        _status = status;
        _sentStatus = true;
    }


    void RequestResponse::writeStatusJSON(HTTPStatus status, const char *message) {
        auto &json = jsonEncoder();
        if (int(status) < 300) {
            json.writeKey("ok"_sl);
            json.writeBool(true);
        } else {
            const char *defaultMessage = mg_get_response_code_text(_conn, int(status));
            json.writeKey("status"_sl);
            json.writeInt(int(status));
            json.writeKey("error"_sl);
            json.writeString(defaultMessage);
            if (message && 0 != strcasecmp(message, defaultMessage)) {
                json.writeKey("reason"_sl);
                json.writeString(message);
            }
        }
    }


    void RequestResponse::writeErrorJSON(C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        writeStatusJSON(errorToStatus(err),
                        (message ? message.asString().c_str() : nullptr));
    }


    void RequestResponse::respondWithStatus(HTTPStatus status, const char *message) {
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


    void RequestResponse::respondWithError(C4Error err) {
        Assert(err.code != 0);
        alloc_slice message = c4error_getMessage(err);
        respondWithStatus(errorToStatus(err),
                          (message ? message.asString().c_str() : nullptr));
    }


    HTTPStatus RequestResponse::errorToStatus(C4Error err) {
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
                    case kC4ErrorDeleted:
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


    void RequestResponse::setHeader(const char *header, const char *value) {
        Assert(!_sentHeaders);
        _headers << header << ": " << value << "\r\n";
    }


    void RequestResponse::addHeaders(map<string, string> headers) {
        for (auto &entry : headers)
            setHeader(entry.first.c_str(), entry.second.c_str());
    }


    void RequestResponse::setContentLength(uint64_t length) {
        Assert(!_chunked);
        Assert(_contentLength < 0);
        setHeader("Content-Length", (int64_t)length);
        _contentLength = (int64_t)length;
    }


    void RequestResponse::setChunked() {
        if (!_chunked) {
            Assert(_contentLength < 0);
            setHeader("Transfer-Encoding", "chunked");
            _chunked = true;
        }
    }


    void RequestResponse::uncacheable() {
        setHeader("Cache-Control", "no-cache, no-store, must-revalidate, private, max-age=0");
        setHeader("Pragma", "no-cache");
        setHeader("Expires", "0");
    }


    void RequestResponse::sendHeaders() {
        if (!_sentHeaders) {
            if (!_sentStatus)
                setStatus(HTTPStatus::OK, "OK");

            _headers << "\r\n";
            auto str = _headers.str();
            mg_write(_conn, str.data(), str.size());
            _headers.clear();
            _sentHeaders = true;
        }
    }


    void RequestResponse::write(slice content) {
        if (!_sentHeaders) {
            if (!_chunked)
                setContentLength(content.size);
            sendHeaders();
        }
        _contentSent += content.size;
        if (_chunked) {
            mg_send_chunk(_conn, (const char*)content.buf, (unsigned)content.size);
        } else {
            Assert(_contentLength >= 0);
            mg_write(_conn, content.buf, content.size);
        }
    }


    void RequestResponse::printf(const char *format, ...) {
        char *str;
        va_list args;
        va_start(args, format);
        size_t length = vasprintf(&str, format, args);
        va_end(args);
        write({str, length});
        free(str);
    }


    fleeceapi::JSONEncoder& RequestResponse::jsonEncoder() {
        setHeader("Content-Type", "application/json");
        if (!_jsonEncoder)
            _jsonEncoder.reset(new fleeceapi::JSONEncoder);
        return *_jsonEncoder;
    }


    void RequestResponse::finish() {
        if (_jsonEncoder) {
            alloc_slice json = _jsonEncoder->finish();
            write(json);
        }
        if (_contentLength < 0 && !_chunked)
            setContentLength(0);
        sendHeaders();
        if (_chunked) {
            mg_send_chunk(_conn, nullptr, 0);
            mg_write(_conn, "\r\n", 2);
        } else {
            Assert(_contentLength == _contentSent);
        }
    }

} }
