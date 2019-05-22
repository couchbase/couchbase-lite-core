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
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "c4Base.h"
#include <functional>
#include <map>
#include <memory>
#include <sstream>


namespace litecore { namespace REST {

    enum class HTTPStatus : int {
        undefined = -1,
        OK = 200,
        Created = 201,
        NoContent = 204,
        NotModified = 304,
        BadRequest = 400,
        Unauthorized = 401,
        Forbidden = 403,
        NotFound = 404,
        MethodNotAllowed = 405,
        NotAcceptable = 406,
        Conflict = 409,
        Gone = 410,
        PreconditionFailed = 412,
        Locked = 423,
        ServerError = 500,
        NotImplemented = 501,
        GatewayError = 502,
    };

    const char* StatusMessage(HTTPStatus);


    /** An incoming HTTP body. */
    class Body {
    public:
        fleece::slice header(const char *name) const;
        fleece::slice operator[] (const char *name) const   {return header(name);}

        bool hasContentType(fleece::slice contentType) const;
        fleece::alloc_slice body() const;
        fleece::Value bodyAsJSON() const;

        // Utilities:
        static std::string urlDecode(const std::string&);
        static std::string urlEncode(const std::string&);

    protected:
        Body() = default;
        Body(fleece::Doc headers, fleece::alloc_slice body)
        :_headers(headers), _body(body)
        { }

        void setHeaders(fleece::Doc doc)            {_headers = doc;}
        void setBody(fleece::alloc_slice body)      {_body = body;}

        fleece::Doc _headers;
        fleece::alloc_slice _body;
        bool _gotBodyFleece {false};
        fleece::Doc _bodyFleece;
    };


    /** An HTTP response from a server, created by specifying a request to send.
        I.e. this is a simple HTTP client API. */
    class Response : public Body {
    public:
        Response(const std::string &method,
                 const std::string &hostname,
                 uint16_t port,
                 const std::string &uri,
                 fleece::slice body =fleece::nullslice);

        Response(const std::string &method,
                 const std::string &hostname,
                 uint16_t port,
                 const std::string &uri,
                 fleece::Doc headers,
                 fleece::slice body =fleece::nullslice);

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

        friend class LWSHTTPClient;
    };

} }
