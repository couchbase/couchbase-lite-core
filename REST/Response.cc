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
#include "HTTPLogic.hh"
#include "Address.hh"
#include "c4ExceptionUtils.hh"
#include "c4Socket.h"
#include "Writer.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "netUtils.hh"
#include "Certificate.hh"
#include "sockpp/mbedtls_context.h"
#include <string>

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace litecore::net;


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
                       Doc headersDict,
                       slice body,
                       crypto::Cert *pinnedServerCert)
    {
        C4Address address = {};
        address.scheme = slice(scheme);
        address.hostname = slice(hostname);
        address.port = port;
        address.path = slice(uri);

        websocket::Headers headers(headersDict.root().asDict());

        unique_ptr<sockpp::mbedtls_context> tlsContext;
        if (pinnedServerCert) {
            tlsContext.reset(new sockpp::mbedtls_context);
            tlsContext->allow_only_certificate(pinnedServerCert->context());
        }

        HTTPLogic logic(repl::Address(address), headers);
        logic.setMethod(MethodNamed(method));
        logic.setContentLength(body.size);

        try {
            HTTPLogic::Disposition disposition;
            do {
                XClientSocket socket(tlsContext.get());
                disposition = logic.sendNextRequest(socket, body);
                if (disposition == HTTPLogic::kSuccess) {
                    _body = socket.readHTTPBody(logic.responseHeaders());
                } else if (disposition == HTTPLogic::kFailure) {
                    auto err = logic.error();
                    if (err)
                        _error = c4error_make((C4ErrorDomain)err->domain, err->code, slice(err->what()));
                } else if (disposition == HTTPLogic::kAuthenticate) {
                    disposition = HTTPLogic::kFailure;
                }
            } while (disposition != HTTPLogic::kSuccess && disposition != HTTPLogic::kFailure);

            // set up the rest of my properties:
            _status = logic.status();
            _statusMessage = string(logic.statusMessage());
            _headers = logic.responseHeaders();
        } catchError(&_error);
    }

} }
