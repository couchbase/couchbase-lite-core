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
#include "HTTPLogic.hh"
#include "Server.hh"
#include "Writer.hh"
#include "PlatformIO.hh"
#include "Error.hh"
#include "Logging.hh"
#include "c4.hh"
#include "netUtils.hh"
#include "TCPSocket.hh"
#include <stdarg.h>

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace net;

#pragma mark - REQUEST:


    Request::Request(Method method, const string &path, const string &queries,
                     websocket::Headers headers, fleece::alloc_slice body)
    :Body(move(headers), body)
    ,_method(method)
    ,_path(path)
    ,_queries(queries)
    { }


    bool Request::readFromHTTP(slice httpData) {
        // <https://tools.ietf.org/html/rfc7230#section-3.1.1>
        _method = Method::None;
        Method method = MethodNamed(httpData.readToDelimiter(" "_sl));
        slice uri = httpData.readToDelimiter(" "_sl);
        slice version = httpData.readToDelimiter("\r\n"_sl);
        if (method == Method::None || uri.size == 0 || !version.hasPrefix("HTTP/"_sl))
            return false;
        
        const uint8_t *q = uri.findByte('?');
        if (q) {
            _queries = string(uri.from(q+1));
            uri = uri.upTo(q);
        } else {
            _queries.clear();
        }
        _path = string(uri);

        if (!HTTPLogic::parseHeaders(httpData, _headers))
            return false;

        _method = method;
        return true;
    }


    string Request::path(int i) const {
        slice path = _path;
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
        return URLDecode(component);
    }

    
    string Request::query(const char *param) const {
        return getURLQueryParam(_queries, param);
    }

    int64_t Request::intQuery(const char *param, int64_t defaultValue) const {
        string val = query(param);
        if (!val.empty()) {
            slice s(val);
            int64_t n = s.readSignedDecimal();
            if (s.size == 0)
                return n;
        }
        return defaultValue;
    }

    bool Request::boolQuery(const char *param, bool defaultValue) const {
        string val = query(param);
        if (val.empty())
            return defaultValue;
        return val != "false" && val != "0";        // same behavior as Obj-C CBL 1.x
    }



#pragma mark - RESPONSE STATUS LINE:


    RequestResponse::RequestResponse(Server *server, std::unique_ptr<net::ResponderSocket> socket)
    :_server(server)
    ,_socket(move(socket))
    {
        auto request = _socket->readToDelimiter("\r\n\r\n"_sl);
        if (!request) {
            handleSocketError();
            return;
        }
        if (!readFromHTTP(request))
            return;
        if (_method == Method::POST || _method == Method::PUT) {
            if (!_socket->readHTTPBody(_headers, _body)) {
                handleSocketError();
                return;
            }
        }
    }


    void RequestResponse::setStatus(HTTPStatus status, const char *message) {
        Assert(!_sentStatus);
        _status = status;
        _statusMessage = message ? message : "";
        sendStatus();
    }


    void RequestResponse::sendStatus() {
        if (_sentStatus)
            return;
        Log("Response status: %d", _status);
        if (_statusMessage.empty()) {
            const char *defaultMessage = StatusMessage(_status);
            if (defaultMessage)
                _statusMessage = defaultMessage;
        }
        string statusLine = format("HTTP/1.0 %d %s\r\n", _status, _statusMessage.c_str());
        _responseHeaderWriter.write(statusLine);
        _sentStatus = true;

        // Add the 'Date:' header:
        char date[50];
        time_t t = time(NULL);
        struct tm tm;
        if (gmtime_r(&t, &tm))
            strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        else
            strlcpy(date, "Thu, 01 Jan 1970 00:00:00 GMT", sizeof(date));
        setHeader("Date", date);
    }


    void RequestResponse::writeStatusJSON(HTTPStatus status, const char *message) {
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
            _jsonEncoder.reset(); // drop any prior buffered output
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


    void RequestResponse::handleSocketError() {
        C4Error err = _socket->error();
        WarnError("Socket error sending response: %s", c4error_descriptionStr(err));
    }


#pragma mark - RESPONSE HEADERS:


    void RequestResponse::setHeader(const char *header, const char *value) {
        sendStatus();
        Assert(!_endedHeaders);
        _responseHeaderWriter.write(slice(header));
        _responseHeaderWriter.write(": "_sl);
        _responseHeaderWriter.write(slice(value));
        _responseHeaderWriter.write("\r\n"_sl);
    }


    void RequestResponse::addHeaders(map<string, string> headers) {
        for (auto &entry : headers)
            setHeader(entry.first.c_str(), entry.second.c_str());
    }


    void RequestResponse::setContentLength(uint64_t length) {
        sendStatus();
        Assert(_contentLength < 0, "Content-Length has already been set");
        Log("Content-Length: %llu", length);
        _contentLength = (int64_t)length;
        char len[20];
        sprintf(len, "%llu", length);
        setHeader("Content-Length", len);
    }


    void RequestResponse::sendHeaders() {
        if (_jsonEncoder)
            setHeader("Content-Type", "application/json");
        _responseHeaderWriter.write("\r\n"_sl);
        if (_socket->write_n(_responseHeaderWriter.finish()) < 0)
            handleSocketError();
        _endedHeaders = true;
    }


#pragma mark - RESPONSE BODY:


    void RequestResponse::uncacheable() {
        setHeader("Cache-Control", "no-cache, no-store, must-revalidate, private, max-age=0");
        setHeader("Pragma", "no-cache");
        setHeader("Expires", "0");
    }


    void RequestResponse::write(slice content) {
        Assert(!_finished);
        _responseWriter.write(content);
    }


    void RequestResponse::printf(const char *format, ...) {
        char *str;
        va_list args;
        va_start(args, format);
        int length = vasprintf(&str, format, args);
        if (length < 0)
            throw bad_alloc();
        va_end(args);
        write({str, size_t(length)});
        free(str);
    }


    fleece::JSONEncoder& RequestResponse::jsonEncoder() {
        if (!_jsonEncoder)
            _jsonEncoder.reset(new fleece::JSONEncoder);
        return *_jsonEncoder;
    }


    void RequestResponse::finish() {
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
        if (_socket->write_n(responseData) < 0)
            handleSocketError();
        _finished = true;
    }


    bool RequestResponse::isValidWebSocketRequest() {
        return header("Connection").caseEquivalent("upgrade"_sl)
            && header("Upgrade").caseEquivalent("websocket"_sl)
            && header("Sec-WebSocket-Version").readDecimal() >= 13
            && header("Sec-WebSocket-Key").size >= 10;
    }


    void RequestResponse::sendWebSocketResponse(const string &protocol) {
        string nonce(header("Sec-WebSocket-Key"));
        setStatus(HTTPStatus::Upgraded, "Upgraded");
        setHeader("Connection", "Upgrade");
        setHeader("Upgrade", "websocket");
        setHeader("Sec-WebSocket-Accept",
                  HTTPLogic::webSocketKeyResponse(nonce).c_str());
        if (!protocol.empty())
            setHeader("Sec-WebSocket-Protocol", protocol.c_str());
        finish();
    }


    void RequestResponse::onClose(std::function<void()> &&callback) {
        _socket->onClose(move(callback));
    }


    unique_ptr<ResponderSocket> RequestResponse::extractSocket() {
        finish();
        return move(_socket);
    }


    string RequestResponse::peerAddress() {
        return _socket->peerAddress();
    }

} }
