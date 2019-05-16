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
#include "civetUtils.hh"

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace litecore::websocket;


    slice Body::header(const char *name) const {
        slice header(name);
        Dict headers = _headers.root().asDict();
        for (Dict::iterator i(headers); i; ++i)
            if (i.keyString().caseEquivalent(header))
                return i.value().asString();
        return nullslice;
    }
    
    
    std::string Body::urlDecode(const std::string &str) {
        std::string result;
        result.reserve(str.size());
        litecore::REST::urlDecode(str.data(), str.size(), result, false);
        return result;
    }


    std::string Body::urlEncode(const std::string &str) {
        std::string result;
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


    Response::Response(const std::string &method,
                       const std::string &hostname,
                       uint16_t port,
                       const std::string &uri,
                       const std::map<std::string, std::string> &headers,
                       fleece::slice body)
    {
        C4Address address = {};
        address.scheme = "http"_sl;
        address.hostname = slice(hostname);
        address.port = port;
        address.path = slice(uri);
        
        Retained<LWSHTTPClient> conn = new LWSHTTPClient(*this, address, method.c_str(),
                                                         alloc_slice(body));
        _error = conn->run();
    }


    Response::Response(const string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri,
                       slice body)
    :Response(method, hostname, port, uri, {}, body)
    { }

} }
