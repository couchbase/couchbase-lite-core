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
#include "XSocket.hh"
#include "Address.hh"
#include "c4ExceptionUtils.hh"
#include "c4Socket.h"
#include "Writer.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "netUtils.hh"
#include "Certificate.hh"
#include "sockpp/mbedtls_context.h"
#include <string>

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace litecore::net;


    slice Body::header(const char *name) const {
        slice header(name);
        Dict headers = _headers.root().asDict();
        for (Dict::iterator i(headers); i; ++i)
            if (i.keyString().caseEquivalent(header))
                return i.value().asString();
        return nullslice;
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
                    _bodyFleece = Doc::fromJSON(b, nullptr);
            }
            _gotBodyFleece = true;
        }
        return _bodyFleece.root();
    }


#pragma mark - RESPONSE:


    Response::Response(const string &scheme,
                       const string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri,
                       Doc headers,
                       slice body,
                       crypto::Cert *pinnedServerCert)
    {
        C4Address address = {};
        address.scheme = slice(scheme);
        address.hostname = slice(hostname);
        address.port = port;
        address.path = slice(uri);

        try {
            HTTPClientSocket socket{repl::Address(address)};
            unique_ptr<sockpp::mbedtls_context> tlsContext;
            if (pinnedServerCert) {
                tlsContext.reset(new sockpp::mbedtls_context);
                tlsContext->allow_only_certificate(pinnedServerCert->context());
                socket.setTLSContext(tlsContext.get());
            }
            socket.connect();
            socket.sendHTTPRequest(method, headers.root().asDict(), body);
            auto response = socket.readHTTPResponse();
            _status = HTTPStatus(response.status);
            _statusMessage = response.message;
            _headers = Doc(response.headers.data());
            _body = socket.readHTTPBody(response.headers);
        } catchError(&_error);
    }

} }
