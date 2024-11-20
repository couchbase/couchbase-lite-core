//
// Request.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Request.hh"
#include "HTTPLogic.hh"
#include "c4ListenerInternal.hh"  // for ListenerLog
#include "Error.hh"
#include "netUtils.hh"
#include "Server.hh"
#include "TCPSocket.hh"
#include "Writer.hh"
#include "slice_stream.hh"
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <memory>
#include <utility>

#ifdef COUCHBASE_ENTERPRISE

#    ifdef _MSC_VER
#        include "PlatformIO.hh"
#    endif

using namespace std;
using namespace std::chrono;
using namespace fleece;

namespace litecore::REST {
    using namespace net;

#    pragma mark - REQUEST:

    Request::Request(Method method, string path, string queries, websocket::Headers headers, fleece::alloc_slice body)
        : Body(std::move(headers), std::move(body))
        , _method(method)
        , _path(std::move(path))
        , _queries(std::move(queries)) {}

    bool Request::readFromHTTP(slice httpData) {
        slice_istream in(httpData);
        // <https://tools.ietf.org/html/rfc7230#section-3.1.1>
        _method        = Method::None;
        Method method  = MethodNamed(in.readToDelimiter(" "_sl));
        slice  uri     = in.readToDelimiter(" "_sl);
        slice  http    = in.readToDelimiter("/"_sl);
        slice  version = in.readToDelimiter("\r\n"_sl);
        if ( method == Method::None || !uri.hasPrefix('/') || http != "HTTP"_sl ) return false;

        if ( version == "1.1" ) _version = HTTP1_1;
        else if ( version == "1.0" )
            _version = HTTP1_0;
        else
            return false;

        if ( !HTTPLogic::parseHeaders(in, _headers) ) return false;

        const uint8_t* q = uri.findByte('?');
        if ( q ) {
            _queries = string(uri.from(q + 1));
            uri      = uri.upTo(q);
        } else {
            _queries.clear();
        }
        _path = string(uri);

        _method = method;
        return true;
    }

    size_t Request::pathLength() const {
        Assert(_path[0] == '/');
        return std::count_if(_path.begin(),
                             _path.end() - _path.ends_with('/'),  // skip any trailing '/'
                             [](char c) { return c == '/'; });
    }

    string Request::path(int i) const {
        slice path = _path;
        Assert(path[0] == '/');
        path.moveStart(1);
        for ( ; i > 0; --i ) {
            auto slash = path.findByteOrEnd('/');
            if ( slash == path.end() ) return "";
            path.setStart(slash + 1);
        }
        auto slash = path.findByteOrEnd('/');
        if ( slash == path.buf ) return "";
        auto component = slice(path.buf, slash).asString();
        return URLDecode(component);
    }

    string Request::query(const char* param) const { return getURLQueryParam(_queries, param); }

    int64_t Request::intQuery(const char* param, int64_t defaultValue) const {
        string val = query(param);
        if ( !val.empty() ) {
            slice_istream s(val);
            int64_t       n = s.readSignedDecimal();
            if ( s.eof() ) return n;
        }
        return defaultValue;
    }

    string Request::uri() const {
        if ( _queries.empty() ) return _path;
        else
            return _path + "?" + _queries;
    }

    uint64_t Request::uintQuery(const char* param, uint64_t defaultValue) const {
        defaultValue = std::min(defaultValue, uint64_t(INT64_MAX));
        return std::max(int64_t(0), intQuery(param, defaultValue));
    }

    bool Request::boolQuery(const char* param, bool defaultValue) const {
        string val = query(param);
        if ( val.empty() ) return defaultValue;
        return val != "false" && val != "0";  // same behavior as Obj-C CBL 1.x
    }

    bool Request::keepAlive() const {
        auto connection = header("Connection");
        return (_version == Request::HTTP1_1) ? (connection != "close") : (connection == "keep-alive");
    }

#    pragma mark - REQUESTRESPONSE:

    RequestResponse::RequestResponse(RequestResponse&&) noexcept            = default;
    RequestResponse& RequestResponse::operator=(RequestResponse&&) noexcept = default;
    RequestResponse::~RequestResponse()                                     = default;

    RequestResponse::RequestResponse(std::unique_ptr<net::ResponderSocket> socket) : _socket(std::move(socket)) {
        auto request = _socket->readToDelimiter("\r\n\r\n"_sl);
        if ( !request ) {
            _error = _socket->error();
            if ( _error == C4Error{WebSocketDomain, 400} ) _error = C4Error{NetworkDomain, kC4NetErrConnectionReset};
            return;
        }
        if ( !readFromHTTP(request) ) {
            _error = C4Error::make(WebSocketDomain, int(HTTPStatus::BadRequest));
            return;
        }
        if ( _method == Method::POST || _method == Method::PUT ) {
            if ( !_socket->readHTTPBody(_headers, _body) ) {
                _error = _socket->error();
                return;
            }
        }

        // Add standard headers:
        char   dateStr[100];
        time_t t = time(nullptr);
        strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t));  // faster than date::format()
        setHeader("Date", dateStr);
    }

    void RequestResponse::setStatus(HTTPStatus status, const char* message) {
        Assert(!_sentStatus);
        _status        = status;
        _statusMessage = message ? message : "";
        sendStatus();
    }

    void RequestResponse::sendStatus() {
        if ( _sentStatus ) return;
        if ( _statusMessage.empty() ) {
            const char* defaultMessage = StatusMessage(_status);
            if ( defaultMessage ) _statusMessage = defaultMessage;
        }
        string statusLine = stringprintf("HTTP/1.1 %d %s\r\n", static_cast<int>(_status), _statusMessage.c_str());
        _responseHeaderWriter.write(statusLine);
        _sentStatus = true;
    }

    void RequestResponse::writeStatusJSON(HTTPStatus status, const char* message) {
        auto& json = jsonEncoder();
        if ( int(status) < 300 ) {
            json.writeKey("ok"_sl);
            json.writeBool(true);
        } else {
            json.writeKey("status"_sl);
            json.writeInt(int(status));
            const char* defaultMessage = StatusMessage(status);
            if ( defaultMessage ) {
                json.writeKey("error"_sl);
                json.writeString(defaultMessage);
            }
            if ( message && *message && defaultMessage && 0 != strcasecmp(message, defaultMessage) ) {
                json.writeKey("reason"_sl);
                json.writeString(message);
            }
        }
    }

    void RequestResponse::writeErrorJSON(C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        writeStatusJSON(errorToStatus(err), (message ? message.asString().c_str() : nullptr));
    }

    void RequestResponse::respondWithStatus(HTTPStatus status, const char* message) {
        setStatus(status, message);
        uncacheable();

        if ( status >= HTTPStatus::OK && status != HTTPStatus::NoContent && status != HTTPStatus::NotModified ) {
            _jsonEncoder.reset();  // drop any prior buffered output
            auto& json = jsonEncoder();
            json.beginDict();
            writeStatusJSON(status, message);
            json.endDict();
        }
    }

    void RequestResponse::respondWithError(C4Error err) {
        Assert(err.code != 0);
        alloc_slice message = c4error_getMessage(err);
        respondWithStatus(errorToStatus(err), message.asString());
    }

    HTTPStatus RequestResponse::errorToStatus(C4Error err) {
        if ( err.code == 0 ) return HTTPStatus::OK;
        HTTPStatus status = HTTPStatus::ServerError;
        // TODO: Add more mappings, and make these table-driven
        switch ( err.domain ) {
            case LiteCoreDomain:
                switch ( err.code ) {
                    case kC4ErrorInvalidParameter:
                    case kC4ErrorBadRevisionID:
                        status = HTTPStatus::BadRequest;
                        break;
                    case kC4ErrorNotADatabaseFile:
                    case kC4ErrorCrypto:
                        status = HTTPStatus::Unauthorized;
                        break;
                    case kC4ErrorNotWriteable:
                        status = HTTPStatus::Forbidden;
                        break;
                    case kC4ErrorNotFound:
                        status = HTTPStatus::NotFound;
                        break;
                    case kC4ErrorConflict:
                        status = HTTPStatus::Conflict;
                        break;
                    case kC4ErrorUnimplemented:
                    case kC4ErrorUnsupported:
                        status = HTTPStatus::NotImplemented;
                        break;
                    case kC4ErrorRemoteError:
                        status = HTTPStatus::GatewayError;
                        break;
                    case kC4ErrorBusy:
                        status = HTTPStatus::Locked;
                        break;
                }
                break;
            case WebSocketDomain:
                if ( err.code < 1000 ) status = HTTPStatus(err.code);
            default:
                break;
        }
        return status;
    }

    void RequestResponse::handleSocketError() {
        if ( C4Error err = _socket->error(); err != _error ) {
            c4log(ListenerLog, kC4LogError, "Socket error sending HTTP response: %s", err.description().c_str());
            if ( !_error ) _error = err;
        }
    }

    void RequestResponse::writeToSocket(slice data) {
        if ( _socket->write_n(data) < 0 ) handleSocketError();
    }

#    pragma mark - RESPONSE HEADERS:

    void RequestResponse::setHeader(slice header, slice value) {
        Assert(!_sentHeaders);
        _responseHeaders.set(header, value);
    }

    void RequestResponse::addHeaders(const map<string, string>& headers) {
        for ( auto& entry : headers ) setHeader(entry.first, entry.second);
    }

    void RequestResponse::setContentLength(uint64_t length) {
        Assert(_contentLength < 0, "Content-Length has already been set");
        Assert(!_chunked);
        _contentLength = (int64_t)length;
        setHeader("Content-Length", _contentLength);
    }

    void RequestResponse::setContentType(std::string_view contentType) { setHeader("Content-Type", contentType); }

    void RequestResponse::sendHeaders() {
        sendStatus();
        if ( _sentHeaders ) return;
        _responseHeaders.forEach([&](slice header, slice value) {
            _responseHeaderWriter.write(header);
            _responseHeaderWriter.write(": "_sl);
            _responseHeaderWriter.write(value);
            _responseHeaderWriter.write("\r\n"_sl);
        });
        _responseHeaderWriter.write("\r\n"_sl);
        writeToSocket(_responseHeaderWriter.finish());
        _sentHeaders = true;
    }

#    pragma mark - RESPONSE BODY:

    void RequestResponse::uncacheable() {
        setHeader("Cache-Control", "no-cache, no-store, must-revalidate, private, max-age=0");
        setHeader("Pragma", "no-cache");
        setHeader("Expires", "0");
    }

    void RequestResponse::setChunked() {
        if ( _method != Method::HEAD ) {
            Assert(_contentLength < 0, "Content-Length has already been set");
            setHeader("Transfer-Encoding", "chunked");
            _streaming = _chunked = true;
        }
    }

    void RequestResponse::write(slice content) {
        Assert(!_finished);
        _responseWriter.write(content);
    }

    void RequestResponse::printf(const char* format, ...) {
        char*   str;
        va_list args;
        va_start(args, format);
        int length = vasprintf(&str, format, args);
        if ( length < 0 ) throw bad_alloc();
        va_end(args);
        write({str, size_t(length)});
        free(str);
    }

    void RequestResponse::flush(size_t minLength) {
        if ( _responseWriter.length() < minLength ) return;
        Assert(!_jsonEncoder);
        if ( !_streaming ) {
            _streaming = true;
            if ( _contentLength < 0 ) setChunked();
        }
        sendHeaders();
        _flush();
    }

    void RequestResponse::_flush() {
        Assert(_sentHeaders);
        if ( _chunked ) {
            // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Transfer-Encoding
            auto chunkSize = _responseWriter.length();
            if ( chunkSize == 0 ) return;
            char   buf[100];
            size_t len = snprintf(buf, sizeof(buf), "%zx\r\n", chunkSize);
            writeToSocket({buf, len});
            write("\r\n");  // this is at the end of the chunk
        }
        _responseWriter.finish([&](slice responseData) {
            if ( _method != Method::HEAD ) writeToSocket(responseData);
        });
    }

    fleece::JSONEncoder& RequestResponse::jsonEncoder() {
        if ( !_jsonEncoder ) {
            _jsonEncoder = std::make_unique<fleece::JSONEncoder>();
            setContentType("application/json");
        }
        return *_jsonEncoder;
    }

    void RequestResponse::finish() {
        if ( _finished || !_socket ) return;

        if ( _jsonEncoder ) {
            alloc_slice json = _jsonEncoder->finish();
            if ( !json ) {
                c4log(ListenerLog, kC4LogError, "HTTP handler failed to encode JSON response: %s (%d)",
                      _jsonEncoder->errorMessage(), _jsonEncoder->error());
                respondWithStatus(HTTPStatus::ServerError, "Internal error");
                return;
            }
            DebugAssert(Doc::fromJSON(json, nullptr), "Response is not valid JSON: %.*s", FMTSLICE(json));
            write(json);
        }

        if ( !_streaming ) {
            if ( _contentLength < 0 ) setContentLength(_responseWriter.length());
            else if ( _method != Method::HEAD )
                Assert(_contentLength == _responseWriter.length());
        }
        sendHeaders();

        _flush();
        if ( _chunked ) {
            writeToSocket("0\r\n\r\n");  // Ending chunk
        }
        _finished = true;
    }

    bool RequestResponse::isValidWebSocketRequest() {
        return header("Connection").caseEquivalent("upgrade"_sl) && header("Upgrade").caseEquivalent("websocket"_sl)
               && slice_istream(header("Sec-WebSocket-Version")).readDecimal() >= 13
               && header("Sec-WebSocket-Key").size >= 10;
    }

    void RequestResponse::sendWebSocketResponse(string_view protocol) {
        string nonce(header("Sec-WebSocket-Key"));
        setStatus(HTTPStatus::Upgraded, "Upgraded");
        setHeader("Connection", "Upgrade");
        setHeader("Upgrade", "websocket");
        setHeader("Sec-WebSocket-Accept", HTTPLogic::webSocketKeyResponse(nonce).c_str());
        if ( !protocol.empty() ) setHeader("Sec-WebSocket-Protocol", protocol);
        finish();
    }

    void RequestResponse::onClose(std::function<void()>&& callback) { _socket->onClose(std::move(callback)); }

    unique_ptr<ResponderSocket> RequestResponse::extractSocket() {
        finish();
        return std::move(_socket);
    }

    string RequestResponse::peerAddress() { return _socket ? _socket->peerAddress() : ""s; }

}  // namespace litecore::REST

#endif
