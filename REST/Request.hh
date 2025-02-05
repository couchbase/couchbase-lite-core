//
// Request.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Response.hh"
#include "HTTPTypes.hh"

#ifdef COUCHBASE_ENTERPRISE

namespace litecore::net {
    class TCPSocket;
}  // namespace litecore::net

namespace litecore::REST {

    /** Incoming HTTP request; read-only */
    class Request : public Body {
      public:
        using Method = net::Method;

        /// Reads an HTTP request from a socket.
        /// If any errors occur, it sets `socketError`.
        explicit Request(net::TCPSocket*);

        bool isValid() const { return _method != Method::None; }

        explicit operator bool() const { return isValid(); }

        Method method() const { return _method; }

        std::string const& path() const LIFETIMEBOUND { return _path; }

        size_t      pathLength() const;
        std::string path(int i) const;

        std::string const& queries() const { return _queries; }

        std::string query(const char* param) const;
        int64_t     intQuery(const char* param, int64_t defaultValue = 0) const;
        uint64_t    uintQuery(const char* param, uint64_t defaultValue = 0) const;
        bool        boolQuery(const char* param, bool defaultValue = false) const;

        std::string uri() const;

        enum HTTPVersion { HTTP1_0, HTTP1_1 };

        HTTPVersion httpVersion() const { return _version; }

        bool keepAlive() const;

        bool isValidWebSocketRequest();

        C4Error socketError() const { return _error; }

      protected:
        Request(Method, std::string path, std::string queries, websocket::Headers headers, alloc_slice body);
        Request() = default;

        bool readFromHTTP(slice httpData);  // data must extend at least to CRLF

        Method      _method{Method::None};
        std::string _path;
        std::string _queries;
        HTTPVersion _version;
        C4Error     _error{};
    };

}  // namespace litecore::REST
#endif
