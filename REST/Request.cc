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
#include "netUtils.hh"
#include "TCPSocket.hh"
#include "slice_stream.hh"
#include <cinttypes>
#include <memory>
#include <utility>

#ifdef COUCHBASE_ENTERPRISE

#    ifdef _MSC_VER
#        include "PlatformIO.hh"
#    endif

using namespace std;
using namespace fleece;

namespace litecore::REST {
    using namespace net;

#    pragma mark - REQUEST:

    Request::Request(Method method, string path, string queries, websocket::Headers headers, fleece::alloc_slice body)
        : Body(std::move(headers), std::move(body))
        , _method(method)
        , _path(std::move(path))
        , _queries(std::move(queries))
        , _version(HTTP1_1) {}

    Request::Request(TCPSocket* socket) {
        alloc_slice request = socket->readToDelimiter("\r\n\r\n"_sl);
        if ( !request ) {
            _error = socket->error();
            if ( _error == C4Error{WebSocketDomain, 400} ) _error = C4Error{NetworkDomain, kC4NetErrConnectionReset};
        } else if ( !readFromHTTP(request) ) {
            _error = C4Error::make(WebSocketDomain, int(HTTPStatus::BadRequest));
        } else if ( _method == Method::POST || _method == Method::PUT ) {
            if ( !socket->readHTTPBody(_headers, _body) ) { _error = socket->error(); }
        }
    }

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

    bool Request::isValidWebSocketRequest() {
        return _method == GET && header("Connection").caseEquivalent("upgrade"_sl)
               && header("Upgrade").caseEquivalent("websocket"_sl)
               && slice_istream(header("Sec-WebSocket-Version")).readDecimal() >= 13
               && header("Sec-WebSocket-Key").size >= 10;
    }

}  // namespace litecore::REST

#endif
