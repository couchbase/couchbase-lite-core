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
#include "TCPSocket.hh"
#include "TLSContext.hh"
#include "HTTPLogic.hh"
#include "Address.hh"
#include "c4Certificate.h"
#include "c4ExceptionUtils.hh"
#include "Writer.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "netUtils.hh"
#include "Certificate.hh"
#include <string>

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {
    using namespace litecore::net;
    using namespace litecore::crypto;


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


    Response::Response(const net::Address &address, Method method)
    :_logic(new HTTPLogic(address))
    {
        _logic->setMethod(method);
    }


    Response::Response(const string &scheme,
                       const std::string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri)
    :Response(Address(slice(scheme), slice(hostname), port, slice(uri)),
              MethodNamed(method))
    { }


    Response::~Response() =default;


    Response& Response::setHeaders(const websocket::Headers &headers) {
        _logic->setHeaders(headers);
        return *this;
    }

    Response& Response::setHeaders(Doc headersDict) {
        return setHeaders( websocket::Headers(headersDict.root().asDict()) );
    }


    Response& Response::setBody(slice body) {
        _requestBody = body;
        _logic->setContentLength(_requestBody.size);
        return *this;
    }

    Response& Response::setAuthHeader(slice authHeader) {
        _logic->setAuthHeader(authHeader);
        return *this;
    }

    TLSContext* Response::tlsContext() {
        if (!_tlsContext)
            _tlsContext = new TLSContext(TLSContext::Client);
        return _tlsContext;
    }

    Response& Response::setTLSContext(net::TLSContext *ctx) {
        _tlsContext = ctx;
        return *this;
    }

    Response& Response::setProxy(const ProxySpec &proxy) {
        _logic->setProxy(proxy);
        return *this;
    }

    Response& Response::allowOnlyCert(slice certData) {
        tlsContext()->allowOnlyCert(certData);
        return *this;
    }

    Response& Response::setRootCerts(slice certsData) {
        tlsContext()->setRootCerts(certsData);
        return *this;
    }

#ifdef COUCHBASE_ENTERPRISE
    Response& Response::allowOnlyCert(C4Cert *cert) {
        Assert(c4cert_isSigned(cert));
        tlsContext()->allowOnlyCert((Cert*)cert);
        return *this;
    }
    
    Response& Response::setRootCerts(C4Cert *cert) {
        Assert(c4cert_isSigned(cert));
        tlsContext()->setRootCerts((Cert*)cert);
        return *this;
    }

    Response& Response::setIdentity(C4Cert *cert, C4KeyPair *key) {
        Assert(c4cert_isSigned(cert));
        Assert(c4keypair_hasPrivateKey(key));
        Retained<Identity> id = new Identity((Cert*)cert, (PrivateKey*)key);
        tlsContext()->setIdentity(id);
        return *this;
    }
#endif


    bool Response::run() {
        if (hasRun())
            return (_error.code == 0);

        try {
            unique_ptr<ClientSocket> socket;
            HTTPLogic::Disposition disposition = HTTPLogic::kFailure;
            do {
                if (disposition != HTTPLogic::kContinue) {
                    socket = make_unique<ClientSocket>(_tlsContext.get());
                    socket->setTimeout(_timeout);
                }
                disposition = _logic->sendNextRequest(*socket, _requestBody);
                switch (disposition) {
                    case HTTPLogic::kSuccess:
                        // On success, read the response body:
                        if (!socket->readHTTPBody(_logic->responseHeaders(), _body)) {
                            _error = socket->error();
                            disposition = HTTPLogic::kFailure;
                        }
                        break;
                    case HTTPLogic::kRetry:
                        break;
                    case HTTPLogic::kContinue:
                        break;
                    case HTTPLogic::kAuthenticate:
                        if (!_logic->authHeader())
                            disposition = HTTPLogic::kFailure;
                        break;
                    case HTTPLogic::kFailure:
                        _error = _logic->error();
                        break;
                }
            } while (disposition != HTTPLogic::kSuccess && disposition != HTTPLogic::kFailure);

            // set up the rest of my properties:
            _status = _logic->status();
            _statusMessage = string(_logic->statusMessage());
            _headers = _logic->responseHeaders();
        } catchError(&_error);
        _logic.reset();
        _tlsContext = nullptr;
        return (_error.code == 0);
    }

} }
