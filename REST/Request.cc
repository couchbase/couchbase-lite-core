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
#include "PlatformIO.hh"
#include <stdarg.h>

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace REST {

#pragma mark - REQUEST:


    Request::Request(mg_connection *conn)
    :Body(conn)
    {
        char date[50];
        time_t curtime = time(NULL);
        gmt_time_string(date, sizeof(date), &curtime);
        setHeader("Date", date);
    }

    
    slice Request::method() const {
        return slice(mg_get_request_info(_conn)->request_method);
    }


    slice Request::path() const {
        return slice(mg_get_request_info(_conn)->request_uri);
    }

    
    string Request::path(int i) const {
        slice path(mg_get_request_info(_conn)->request_uri);
        assert(path[0] == '/');
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


    fleeceapi::JSONEncoder& Request::jsonEncoder() {
        setHeader("Content-Type", "application/json");
        if (!_jsonEncoder)
            _jsonEncoder.reset(new fleeceapi::JSONEncoder);
        return *_jsonEncoder;
    }


    void Request::finish() {
        if (_jsonEncoder) {
            alloc_slice json = _jsonEncoder->finish();
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
