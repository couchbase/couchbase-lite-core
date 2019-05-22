//
// Response.cc
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

#include "Response.hh"
#include "LWSHTTPClient.hh"
#include "c4Socket.h"
#include "Writer.hh"
#include "StringUtil.hh"
#include "netUtils.hh"

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace litecore::websocket;


    const char* StatusMessage(HTTPStatus code) {
        static const struct {HTTPStatus code; const char* message;} kMessages[] = {
            {HTTPStatus::OK, "OK"},
            {HTTPStatus::Created, "Created"},
            {HTTPStatus::NoContent, "No Content"},
            {HTTPStatus::BadRequest, "Invalid Request"},
            {HTTPStatus::Unauthorized, "Unauthorized"},
            {HTTPStatus::Forbidden, "Forbidden"},
            {HTTPStatus::NotFound, "Not Found"},
            {HTTPStatus::MethodNotAllowed, "Method Not Allowed"},
            {HTTPStatus::NotAcceptable, "Not Acceptable"},
            {HTTPStatus::Conflict, "Conflict"},
            {HTTPStatus::Gone, "Gone"},
            {HTTPStatus::PreconditionFailed, "Precondition Failed"},
            {HTTPStatus::ServerError, "Internal Server Error"},
            {HTTPStatus::NotImplemented, "Not Implemented"},
            {HTTPStatus::GatewayError, "Bad Gateway"},
            {HTTPStatus::undefined, nullptr}
        };

        for (unsigned i = 0; kMessages[i].message; ++i) {
            if (kMessages[i].code == code)
                return kMessages[i].message;
        }
        return nullptr;
    }


    slice Body::header(const char *name) const {
        slice header(name);
        Dict headers = _headers.root().asDict();
        for (Dict::iterator i(headers); i; ++i)
            if (i.keyString().caseEquivalent(header))
                return i.value().asString();
        return nullslice;
    }
    
    
    string Body::urlDecode(const string &str) {
        string result;
        result.reserve(str.size());
        litecore::REST::urlDecode(str.data(), str.size(), result, false);
        return result;
    }


    string Body::urlEncode(const string &str) {
        string result;
        result.reserve(str.size() + 16);
        litecore::REST::urlEncode(str.data(), str.size(), result, false);
        return result;
    }


    bool Body::hasContentType(slice contentType) const {
        slice actualType = header("Content-Type");
        return actualType.size >= contentType.size
            && memcmp(actualType.buf, contentType.buf, contentType.size) == 0
            && (actualType.size == contentType.size || actualType[contentType.size] == ';');
    }


    alloc_slice Body::body() const {
        return _body;
    }


    Value Body::bodyAsJSON() const {
        if (!_gotBodyFleece) {
            if (hasContentType("application/json"_sl)) {
                alloc_slice b = body();
                if (b)
                    const_cast<Body*>(this)->_bodyFleece =
                    Doc::fromJSON(b, nullptr);
            }
            const_cast<Body*>(this)->_gotBodyFleece = true;
        }
        return _bodyFleece.root();
    }


#pragma mark - RESPONSE:


    Response::Response(const string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri,
                       Doc headers,
                       slice body)
    {
        C4Address address = {};
        address.scheme = "http"_sl;
        address.hostname = slice(hostname);
        address.port = port;
        address.path = slice(uri);
        
        Retained<LWSHTTPClient> conn = new LWSHTTPClient(*this);
        conn->connect(address, method.c_str(), headers, alloc_slice(body));
        _error = conn->run();
    }


    Response::Response(const string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri,
                       slice body)
    :Response(method, hostname, port, uri, nullptr, body)
    { }

} }
