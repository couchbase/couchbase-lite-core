//
// Response.hh
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
#include "Headers.hh"
#include "HTTPTypes.hh"
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "c4Base.h"
#include <functional>
#include <map>
#include <memory>
#include <sstream>

namespace litecore { namespace crypto {
    class Cert;
} }

namespace sockpp {
    class tls_context;
}

namespace litecore { namespace REST {

    /** An incoming HTTP body. */
    class Body {
    public:
        using HTTPStatus = net::HTTPStatus;

        fleece::slice header(const char *name) const        {return _headers[fleece::slice(name)];}
        fleece::slice operator[] (const char *name) const   {return header(name);}

        bool hasContentType(fleece::slice contentType) const;
        fleece::alloc_slice body() const;
        fleece::Value bodyAsJSON() const;

    protected:
        Body() = default;
        Body(websocket::Headers headers, fleece::alloc_slice body)
        :_headers(headers), _body(body)
        { }

        void setHeaders(websocket::Headers h)       {_headers = h;}
        void setBody(fleece::alloc_slice body)      {_body = body;}

        websocket::Headers _headers;
        fleece::alloc_slice _body;
        mutable bool _gotBodyFleece {false};
        mutable fleece::Doc _bodyFleece;
    };


    /** An HTTP response from a server, created by specifying a request to send.
        I.e. this is a simple HTTP client API. */
    class Response : public Body {
    public:
        Response(const std::string &scheme,
                 const std::string &method,
                 const std::string &hostname,
                 uint16_t port,
                 const std::string &uri,
                 fleece::Doc headers,
                 fleece::slice body =fleece::nullslice,
                 crypto::Cert *pinnedServerCert =nullptr);

        Response(const std::string &scheme,
                 const std::string &method,
                 const std::string &hostname,
                 uint16_t port,
                 const std::string &uri,
                 fleece::slice body =fleece::nullslice)
        :Response(scheme, method, hostname, port, uri, nullptr, body, nullptr)
        { }

        Response(const std::string &method,
                 const std::string &hostname,
                 uint16_t port,
                 const std::string &uri,
                 fleece::Doc headers,
                 fleece::slice body =fleece::nullslice)
        :Response("http", method, hostname, port, uri, nullptr, body, nullptr)
        { }

        explicit operator bool() const      {return _error.code == 0;}

        C4Error error() const               {return _error;}
        HTTPStatus status() const           {return _status;}
        std::string statusMessage() const   {return _statusMessage;}

    protected:
        void setStatus(int status, const std::string &msg) {
            _status = (HTTPStatus)status;
            _statusMessage = msg;
        }

    private:
        HTTPStatus _status {HTTPStatus::undefined};
        std::string _statusMessage;
        C4Error _error {};
    };

} }
