//
// Response.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Response.hh"
#include "TCPSocket.hh"
#include "TLSContext.hh"
#include "HTTPLogic.hh"
#include "Address.hh"
#include "c4Certificate.hh"
#include "c4ExceptionUtils.hh"
#include "Error.hh"
#include "Certificate.hh"
#include "fleece/FLExpert.h"
#include <string>

using namespace std;
using namespace fleece;

namespace litecore::REST {
    using namespace litecore::net;
    using namespace litecore::crypto;

    alloc_slice Body::body() const { return _body; }

    Value Body::bodyAsJSON() const {
        if ( !_gotBodyFleece ) {
            if ( auto type = header("Content-Type"); type.hasPrefix("application/json") ) {
                if ( alloc_slice b = body() ) {
                    FLError err{};
                    if ( type.hasPrefix("application/json5") || type.hasPrefix("application/jsonc") )
                        b = FLJSON5_ToJSON(slice(b), nullptr, nullptr, &err);
                    if ( b ) _bodyFleece = Doc::fromJSON(b, &err);
                    if ( !_bodyFleece ) Warn("HTTP Body has unparseable JSON (%d): %.*s", err, FMTSLICE(_body));
                }
            }
            _gotBodyFleece = true;
        }
        return _bodyFleece.root();
    }

#pragma mark - RESPONSE:

    Response::Response(const net::Address& address, Method method) : _logic(new HTTPLogic(address)) {
        _logic->setMethod(method);
    }

    Response::Response(const string& scheme, const std::string& method, const string& hostname, uint16_t port,
                       const string& uri)
        : Response(Address(slice(scheme), slice(hostname), port, slice(uri)), MethodNamed(method)) {}

    Response::~Response() = default;

    Response& Response::setHeaders(const websocket::Headers& headers) {
        _logic->setHeaders(headers);
        return *this;
    }

    Response& Response::setHeaders(const Doc& headersDict) {
        return setHeaders(websocket::Headers(headersDict.root().asDict()));
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
        if ( !_tlsContext ) _tlsContext = new TLSContext(TLSContext::Client);
        return _tlsContext;
    }

    Response& Response::setTLSContext(net::TLSContext* ctx) {
        _tlsContext = ctx;
        return *this;
    }

    Response& Response::setProxy(const ProxySpec& proxy) {
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
    Response& Response::allowOnlyCert(C4Cert* cert) {
        tlsContext()->allowOnlyCert(cert->assertSignedCert());
        return *this;
    }

    Response& Response::setRootCerts(C4Cert* cert) {
        tlsContext()->setRootCerts(cert->assertSignedCert());
        return *this;
    }

    Response& Response::setIdentity(C4Cert* cert, C4KeyPair* key) {
        Assert(key->hasPrivateKey());
        Retained<Identity> id = new Identity(cert->assertSignedCert(), key->getPrivateKey());
        tlsContext()->setIdentity(id);
        return *this;
    }
#endif


    bool Response::run() {
        if ( hasRun() ) return (_error.code == 0);

        try {
            unique_ptr<ClientSocket> socket;
            HTTPLogic::Disposition   disposition = HTTPLogic::kFailure;
            do {
                if ( disposition != HTTPLogic::kContinue ) {
                    socket = make_unique<ClientSocket>(_tlsContext.get());
                    socket->setTimeout(_timeout);
                }
                disposition = _logic->sendNextRequest(*socket, _requestBody);
                switch ( disposition ) {
                    case HTTPLogic::kSuccess:
                        // On success, read the response body:
                        if ( !socket->readHTTPBody(_logic->responseHeaders(), _body) ) {
                            _error      = socket->error();
                            disposition = HTTPLogic::kFailure;
                        }
                        break;
                    case HTTPLogic::kRetry:
                    case HTTPLogic::kContinue:
                        break;
                    case HTTPLogic::kAuthenticate:
                        if ( !_logic->authHeader() ) disposition = HTTPLogic::kFailure;
                        break;
                    case HTTPLogic::kFailure:
                        _error = _logic->error();
                        break;
                }
            } while ( disposition != HTTPLogic::kSuccess && disposition != HTTPLogic::kFailure );

            // set up the rest of my properties:
            _status        = _logic->status();
            _statusMessage = string(_logic->statusMessage());
            _headers       = _logic->responseHeaders();
        }
        catchError(&_error);
        _logic.reset();
        _tlsContext = nullptr;
        return (_error.code == 0);
    }

}  // namespace litecore::REST
