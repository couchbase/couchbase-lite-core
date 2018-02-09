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
#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4Base.h"
#include <functional>
#include <map>
#include <memory>
#include <sstream>

struct mg_connection;

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
        Conflict = 409,
        PreconditionFailed = 412,
        Locked = 423,
        ServerError = 500,
        NotImplemented = 501,
        GatewayError = 502,
    };

    /** An incoming HTTP body. */
    class Body {
    public:
        fleece::slice header(const char *name) const;
        fleece::slice operator[] (const char *name) const   {return header(name);}

        bool hasContentType(fleece::slice contentType) const;
        fleece::alloc_slice body() const;
        fleeceapi::Value bodyAsJSON() const;

        // Utilities:
        static std::string urlDecode(const std::string&);
        static std::string urlEncode(const std::string&);

    protected:
        Body(mg_connection*);

        mg_connection* const _conn;
        bool _gotBody {false};
        fleece::alloc_slice _body;
        bool _gotBodyFleece {false};
        fleece::alloc_slice _bodyFleece;
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
                 const std::map<std::string, std::string> &headers,
                 fleece::slice body =fleece::nullslice);

        ~Response();

        explicit operator bool() const      {return _conn != nullptr;}

        HTTPStatus status() const;
        std::string statusMessage() const;

    private:
        std::string _errorMessage;
        int _errorCode;
    };

} }
