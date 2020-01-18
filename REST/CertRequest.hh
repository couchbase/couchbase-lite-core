//
// CertRequest.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Certificate.hh"
#include "Address.hh"
#include "fleece/Fleece.hh"
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
