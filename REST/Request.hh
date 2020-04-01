//
// Request.hh
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

#pragma once
#include "Response.hh"
#include "HTTPTypes.hh"
#include "PlatformCompat.hh"
#include "StringUtil.hh"
#include "Writer.hh"

namespace litecore { namespace net {
    class ResponderSocket;
} }

namespace litecore { namespace REST {
    class Server;

    /** Incoming HTTP request; read-only */
    class Request : public Body {
    public:
        using Method = net::Method;

        explicit Request(fleece::slice httpData);

        bool isValid() const                    {return _method != Method::None;}
        operator bool () const                  {return isValid();}
        
        Method method() const                   {return _method;}

        std::string path() const                {return _path;}
        std::string path(int i) const;

        std::string query(const char *param) const;
        int64_t intQuery(const char *param, int64_t defaultValue =0) const;
        bool boolQuery(const char *param, bool defaultValue =false) const;

    protected:
        friend class Server;
        
        Request(Method, const std::string &path, const std::string &queries,
                websocket::Headers headers, fleece::alloc_slice body);
        Request() { }

        bool readFromHTTP(fleece::slice httpData);      // data must extend at least to CRLF

        Method _method {Method::None};
        std::string _path;
        std::string _queries;
    };


    /** Incoming HTTP request (inherited from Request), plus setters for the response. */
    class RequestResponse : public Request {
    public:
        // Response status:

        void respondWithStatus(HTTPStatus, const char *message =nullptr);
        void respondWithError(C4Error);

        void setStatus(HTTPStatus status, const char *message);

        HTTPStatus status() const                             {return _status;}

        HTTPStatus errorToStatus(C4Error);

        // Response headers:

        void setHeader(const char *header, const char *value);

        void setHeader(const char *header, int64_t value) {
            setHeader(header, std::to_string(value).c_str());
        }

        void addHeaders(std::map<std::string, std::string>);

        // Response body:

        void setContentLength(uint64_t length);
        void uncacheable();

        void write(fleece::slice);
        void write(const char *content)                     {write(fleece::slice(content));}
        void printf(const char *format, ...) __printflike(2, 3);

        fleece::JSONEncoder& jsonEncoder();

        void writeStatusJSON(HTTPStatus status, const char *message =nullptr);
        void writeErrorJSON(C4Error);

        // Must be called after everything's written:
        void finish();

        // WebSocket stuff:

        bool isValidWebSocketRequest();

        void sendWebSocketResponse(const std::string &protocol);

        void onClose(std::function<void()> &&callback);

        std::unique_ptr<net::ResponderSocket> extractSocket();

        std::string peerAddress();

    protected:
        RequestResponse(Server *server, std::unique_ptr<net::ResponderSocket>);
        void sendStatus();
        void sendHeaders();
        void handleSocketError();

    private:
        friend class Server;

        fleece::Retained<Server> _server;
        std::unique_ptr<net::ResponderSocket> _socket;
        C4Error _error {};

        std::vector<fleece::alloc_slice> _requestBody;

        HTTPStatus _status {HTTPStatus::OK};        // Response status code
        std::string _statusMessage;                 // Response custom status message
        bool _sentStatus {false};                   // Sent the response line yet?

        fleece::Writer _responseHeaderWriter;
        bool _endedHeaders {false};                 // True after headers are ended
        int64_t _contentLength {-1};                // Content-Length, once it's set

        fleece::Writer _responseWriter;             // Output stream for response body
        std::unique_ptr<fleece::JSONEncoder> _jsonEncoder;  // Used for writing JSON to response
        fleece::alloc_slice _responseBody;          // Finished response body
        fleece::slice _unsentBody;                  // Unsent portion of _responseBody
        bool _finished {false};                     // Finished configuring the response?
    };

} }
