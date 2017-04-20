//
//  Request.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Request.hh"
#include "Writer.hh"
#include "civetUtils.hh"
#include "civetweb.h"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace REST {

#pragma mark - REQUEST:


    Request::Request(Server *server, mg_connection *conn)
    :_server(server)
    ,_conn(conn)
    {
        char date[50];
        time_t curtime = time(NULL);
        gmt_time_string(date, sizeof(date), &curtime);
        setHeader("Date", date);
    }


    slice Request::method() const {
        return slice(mg_get_request_info(_conn)->request_method);
    }
    
    slice Request::header(const char *header) const {
        return slice(mg_get_header(_conn, header));
    }


    slice Request::path() const {
        return slice(mg_get_request_info(_conn)->request_uri);
    }

    
    slice Request::path(int i) const {
        slice path(mg_get_request_info(_conn)->request_uri);
        assert(path[0] == '/');
        path.moveStart(1);
        for (; i > 0; --i) {
            auto slash = path.findByteOrEnd('/');
            if (slash == path.end())
                return nullslice;
            path.setStart(slash + 1);
        }
        auto slash = path.findByteOrEnd('/');
        if (slash == path.buf)
            return nullslice;
        return slice(path.buf, slash);
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


    std::string Request::urlDecode(const std::string &str) {
        std::string result;
        result.reserve(str.size());
        litecore::REST::urlDecode(str.data(), str.size(), result, false);
        return result;
    }


    std::string Request::urlEncode(const std::string &str) {
        std::string result;
        result.reserve(str.size() + 16);
        litecore::REST::urlEncode(str.data(), str.size(), result, false);
        return result;
    }


    bool Request::hasContentType(slice contentType) const {
        slice actualType = header("Content-Type");
        return actualType.size >= contentType.size
            && memcmp(actualType.buf, contentType.buf, contentType.size) == 0
            && (actualType.size == contentType.size || actualType[contentType.size] == ';');
    }


    alloc_slice Request::requestBody() const {
        if (!_gotRequestBody) {
            fleece::Writer writer;
            int bytesRead;
            do {
                char buf[1024];
                bytesRead = mg_read(_conn, buf, sizeof(buf));
                writer.write(buf, bytesRead);
            } while (bytesRead > 0);
            if (bytesRead < 0)
                return {};
            alloc_slice body = writer.extractOutput();
            if (body.size == 0)
                body.reset();
            const_cast<Request*>(this)->_requestBody = body;
            const_cast<Request*>(this)->_gotRequestBody = true;
        }
        return _requestBody;
    }


    Value Request::requestJSON() const {
        if (!_gotRequestBodyFleece) {
            if (hasContentType("application/json"_sl)) {
                alloc_slice body = requestBody();
                if (body)
                    const_cast<Request*>(this)->_requestBodyFleece =
                                                        JSONEncoder::convertJSON(body, nullptr);
            }
            const_cast<Request*>(this)->_gotRequestBodyFleece = true;
        }
        return _requestBodyFleece ? Value::fromData(_requestBodyFleece) : nullptr;
    }


#pragma mark - RESPONSE:


    void Request::setStatus(unsigned status, const char *message) {
        assert(!_sentStatus);
        mg_printf(_conn, "HTTP/1.1 %d %s\r\n", status, (message ? message : ""));
        _status = status;
        _sentStatus = true;
    }


    void Request::respondWithError(int status, const char *message) {
        assert(!_sentStatus);
        mg_send_http_error(_conn, status, "%s", message);
        _status = status;
        _sentStatus = true;
        _sentHeaders = true;
    }


    void Request::respondWithError(C4Error err) {
        assert(err.code != 0);
        alloc_slice message = c4error_getMessage(err);
        int status = 500;
        // TODO: Add more mappings, and make these table-driven
        switch (err.domain) {
            case LiteCoreDomain:
                switch (err.code) {
                    case kC4ErrorInvalidParameter:
                    case kC4ErrorBadRevisionID:
                        status = 400; break;
                    case kC4ErrorNotADatabaseFile:
                    case kC4ErrorCrypto:
                        status = 401; break;
                    case kC4ErrorNotWriteable:
                        status = 403; break;
                    case kC4ErrorNotFound:
                    case kC4ErrorDeleted:
                        status = 404; break;
                    case kC4ErrorConflict:
                        status = 409; break;
                    case kC4ErrorUnimplemented:
                    case kC4ErrorUnsupported:
                        status = 501; break;
                    case kC4ErrorRemoteError:
                        status = 502; break;
                }
                break;
            default:
                break;
        }
        respondWithError(status, message.asString().c_str());
    }


    void Request::setHeader(const char *header, const char *value) {
        assert(!_sentHeaders);
        _headers << header << ": " << value << "\r\n";
    }


    void Request::addHeaders(map<string, string> headers) {
        for (auto &entry : headers)
            setHeader(entry.first.c_str(), entry.second.c_str());
    }


    void Request::setContentLength(uint64_t length) {
        assert(!_chunked);
        assert(_contentLength < 0);
        setHeader("Content-Length", (int64_t)length);
        _contentLength = (int64_t)length;
    }


    void Request::setChunked() {
        if (!_chunked) {
            assert(_contentLength < 0);
            setHeader("Transfer-Encoding", "chunked");
            _chunked = true;
        }
    }


    void Request::sendHeaders() {
        if (!_sentHeaders) {
            if (!_sentStatus)
                setStatus(200, "OK");

            _headers << "\r\n";
            auto str = _headers.str();
            mg_write(_conn, str.data(), str.size());
            _headers.clear();
            _sentHeaders = true;
        }
    }


    void Request::write(slice content) {
        if (!_sentHeaders) {
            if (!_chunked)
                setContentLength(content.size);
            sendHeaders();
        }
        _contentSent += content.size;
        if (_chunked) {
            mg_send_chunk(_conn, (const char*)content.buf, (unsigned)content.size);
        } else {
            assert(_contentLength >= 0);
            mg_write(_conn, content.buf, content.size);
        }
    }


    void Request::printf(const char *format, ...) {
        char *str;
        va_list args;
        va_start(args, format);
        size_t length = vasprintf(&str, format, args);
        va_end(args);
        write({str, length});
        free(str);
    }


    fleeceapi::JSONEncoder& Request::json() {
        setHeader("Content-Type", "application/json");
        if (!_json)
            _json.reset(new fleeceapi::JSONEncoder);
        return *_json;
    }


    void Request::finish() {
        if (_json) {
            alloc_slice json = _json->finish();
            write(json);
        }
        sendHeaders();
        assert(_contentLength < 0 || _contentLength == _contentSent);
        if (_chunked) {
            mg_send_chunk(_conn, nullptr, 0);
            mg_write(_conn, "\r\n", 2);
        }
    }

} }
