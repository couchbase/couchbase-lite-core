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
#include "Writer.hh"
#include <functional>
#include <map>
#include <memory>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore::net {
    class ResponderSocket;
}  // namespace litecore::net

namespace litecore::REST {
    class Server;

    /** Incoming HTTP request; read-only */
    class Request : public Body {
      public:
        using Method = net::Method;

        explicit Request(slice httpData);

        bool isValid() const { return _method != Method::None; }

        explicit operator bool() const { return isValid(); }

        Method method() const { return _method; }

        std::string const& path() const { return _path; }

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

      protected:
        Request(Method, std::string path, std::string queries, websocket::Headers headers, alloc_slice body);
        Request() = default;

        bool readFromHTTP(slice httpData);  // data must extend at least to CRLF

        Method      _method{Method::None};
        std::string _path;
        std::string _queries;
        HTTPVersion _version;
    };

    /** Incoming HTTP request (inherited from Request), plus setters for the response. */
    class RequestResponse : public Request {
      public:
        RequestResponse(std::unique_ptr<net::ResponderSocket>);
        RequestResponse(RequestResponse&&) noexcept;
        RequestResponse& operator=(RequestResponse&&) noexcept;
        ~RequestResponse();

        // Response status:

        void respondWithStatus(HTTPStatus, const char* message = nullptr);

        void respondWithStatus(HTTPStatus status, std::string const& message) {
            respondWithStatus(status, message.c_str());
        }

        void respondWithError(C4Error);

        void setStatus(HTTPStatus status, const char* message);

        HTTPStatus status() const { return _status; }

        static HTTPStatus errorToStatus(C4Error);

        // Response headers:

        void setHeader(slice header, slice value);

        void setHeader(slice header, int64_t value) { setHeader(header, std::to_string(value)); }

        void addHeaders(const std::map<std::string, std::string>&);

        websocket::Headers const& responseHeaders() const LIFETIMEBOUND { return _responseHeaders; }

        /// Enables HTTP 'chunked' transfer encoding.
        void setChunked();

        void setContentType(std::string_view contentType);

        // Response body:

        void setContentLength(uint64_t length);
        void uncacheable();

        void write(slice);

        void write(const char* content) { write(slice(content)); }

        void printf(const char* format, ...) __printflike(2, 3);

        JSONEncoder& jsonEncoder() LIFETIMEBOUND;

        void writeStatusJSON(HTTPStatus status, const char* message = nullptr);
        void writeErrorJSON(C4Error);

        /// Flushes output so far to socket. The first call will send the status line + headers first.
        /// If `setContentLength` has not been called, will add a `Connection: close` header.
        /// @param minLength  If given, flush only if this many bytes are buffered.
        /// @warning Not compatible with use of jsonEncoder().
        void flush(size_t minLength = 0);

        /// MUST be called after everything's written.
        void finish();

        bool finished() const { return _finished || !_socket; }

        C4Error socketError() const { return _error; }

        // WebSocket stuff:

        bool isValidWebSocketRequest();

        void sendWebSocketResponse(std::string_view protocol);

        void onClose(std::function<void()>&& callback);

        std::unique_ptr<net::ResponderSocket> extractSocket();

        bool hasSocket() const { return _socket != nullptr; }

        std::string peerAddress();

      protected:
        void sendStatus();
        void sendHeaders();
        void writeToSocket(slice);
        void _flush();
        void handleSocketError();

      private:
        std::unique_ptr<net::ResponderSocket> _socket;
        C4Error                               _error{};
        std::vector<alloc_slice>              _requestBody;
        HTTPStatus                            _status{HTTPStatus::OK};  // Response status code
        std::string                           _statusMessage;           // Response custom status message
        bool                                  _sentStatus{false};       // Sent the response line yet?
        Writer                                _responseHeaderWriter;
        websocket::Headers                    _responseHeaders;
        bool                                  _sentHeaders{false};  // True after headers are ended
        int64_t                               _contentLength{-1};   // Content-Length, once it's set
        bool                         _streaming{false};  // If true, content is being streamed, no Content-Length header
        bool                         _chunked{false};    // True if using chunked transfer encoding
        Writer                       _responseWriter;    // Output stream for response body
        std::unique_ptr<JSONEncoder> _jsonEncoder;       // Used for writing JSON to response
        alloc_slice                  _responseBody;      // Finished response body
        slice                        _unsentBody;        // Unsent portion of _responseBody
        bool                         _finished{false};   // Finished configuring the response?
    };

}  // namespace litecore::REST

#endif
