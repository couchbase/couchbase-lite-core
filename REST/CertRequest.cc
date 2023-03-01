//
// CertRequest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "CertRequest.hh"
#include "Response.hh"
#include "Headers.hh"
#include "HTTPLogic.hh"
#include "c4ReplicatorTypes.h"
#include "Error.hh"

namespace litecore::REST {
    using namespace fleece;
    using namespace litecore::net;
    using namespace litecore::websocket;
    using namespace litecore::crypto;

    CertRequest::CertRequest() {}

    void CertRequest::start(CertSigningRequest* csr, const Address& address, AllocedDict netConfig,
                            CompletionRoutine onComplete) {
        Assert(!_response);
        _response.reset(new Response(address, net::POST));
        _csr        = csr;
        _onComplete = onComplete;

        Dict  authDict = netConfig[kC4ReplicatorOptionAuthentication].asDict();
        slice authType = authDict[kC4ReplicatorAuthType].asString();
        if ( authType == slice(kC4AuthTypeBasic) ) {
            slice username = authDict[kC4ReplicatorAuthUserName].asString();
            slice password = authDict[kC4ReplicatorAuthPassword].asString();
            if ( username && password ) _response->setAuthHeader(HTTPLogic::basicAuth(username, password));
        }

        if ( slice roots = netConfig[kC4ReplicatorOptionRootCerts].asData(); roots ) _response->setRootCerts(roots);
        if ( slice pinned = netConfig[kC4ReplicatorOptionPinnedServerCert].asData(); pinned )
            _response->allowOnlyCert(pinned);

        Headers headers(netConfig[kC4ReplicatorOptionExtraHeaders].asDict());
        headers.add("Content-Type"_sl, "application/json"_sl);
        _response->setHeaders(headers);

        //      _response->setProxy(proxy); // TODO: Proxy support

        // There is no standard I know of for sending CSRs over HTTP, but I'm roughly following
        // <https://github.com/cloudflare/cfssl/blob/master/doc/api/endpoint_sign.txt>

        JSONEncoder body;
        body.beginDict();
        body.writeKey("certificate_request"_sl);
        body.writeString(alloc_slice(csr->data(KeyFormat::PEM)));
        body.endDict();
        _response->setBody(body.finish());

        _thread = std::thread(std::bind(&CertRequest::_run, this));
        retain(this);  // keep myself alive until I complete
    }

    void CertRequest::_run() {
        //---- This runs on a background thread ---
        Retained<Cert> cert;
        C4Error        error;

        if ( !_response->run() ) {
            error = _response->error();
        } else if ( !IsSuccess(_response->status()) ) {
            error = c4error_make(WebSocketDomain, int(_response->status()), slice(_response->statusMessage()));
        } else {
            Dict  body    = _response->bodyAsJSON().asDict();
            Dict  result  = body["result"].asDict();
            slice certPEM = result["certificate"].asString();
            if ( certPEM ) {
                try {
                    cert  = new Cert(certPEM);
                    error = {};
                    // Success! Now sanity-check:
                    if ( cert->subjectPublicKey()->data() != _csr->subjectPublicKey()->data() ) {
                        cert  = nullptr;
                        error = c4error_make(LiteCoreDomain, kC4ErrorRemoteError,
                                             "Certificate from server does not match requested"_sl);
                    }
                } catch ( const litecore::error& x ) {
                    error = c4error_make(LiteCoreDomain, kC4ErrorRemoteError,
                                         "Invalid certificate data in server response"_sl);
                }
            } else {
                error = c4error_make(LiteCoreDomain, kC4ErrorRemoteError, "Missing certificate in server response"_sl);
            }
        }

        // Finally call the completion routine:
        _onComplete(cert, error);

        _thread.detach();
        release(this);  // balances retain() at end of start()
    }

}  // namespace litecore::REST
