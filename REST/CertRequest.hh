//
// CertRequest.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Certificate.hh"
#include "Address.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include <functional>
#include <memory>
#include <thread>

namespace litecore::REST {
    class Response;


    /** Sends an HTTP request to a Certificate Authority, to have a certificate signed. */
    class CertRequest : public fleece::RefCounted {
    public:
        CertRequest();

        using CompletionRoutine = std::function<void(crypto::Cert*, C4Error)>;

        void start(crypto::CertSigningRequest *csr NONNULL,
                   const net::Address &address,
                   fleece::AllocedDict networkConfig,
                   CompletionRoutine);

    private:
        void _run();

        fleece::Retained<crypto::CertSigningRequest> _csr;

        std::unique_ptr<Response> _response;
        std::thread _thread;
        CompletionRoutine _onComplete;
    };

}
