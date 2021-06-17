//
// TLSContext.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "TLSContext.hh"
#include "Certificate.hh"
#include "Logging.hh"
#include "WebSocketInterface.hh"
#include "sockpp/mbedtls_context.h"
#include "mbedtls/debug.h"
#include <string>

using namespace std;
using namespace sockpp;
using namespace fleece;

namespace litecore { namespace net {
    using namespace crypto;


    TLSContext::TLSContext(role_t role)
    :_context(new mbedtls_context(role == Client ? tls_context::CLIENT : tls_context::SERVER))
    ,_role(role)
    {
#ifdef ROOT_CERT_LOOKUP_AVAILABLE
        _context->set_root_cert_locator([this](string certStr, string &rootStr) {
            return findSigningRootCert(certStr, rootStr);
        });
#endif

        // Set up mbedTLS logging. mbedTLS log levels are numbered:
        //   0 No debug
        //   1 Error
        //   2 State change
        //   3 Informational
        //   4 Verbose
        int mbedLogLevel = 1;
        if (auto logLevel = TLSLogDomain.effectiveLevel(); logLevel == LogLevel::Verbose)
            mbedLogLevel = 2;
        else if (logLevel == LogLevel::Debug)
            mbedLogLevel = 4;
        _context->set_logger(mbedLogLevel, [=](int level, const char *filename, int line,
                                               const char *message) {
            static const LogLevel kLogLevels[] = {LogLevel::Error, LogLevel::Error,
                LogLevel::Verbose, LogLevel::Verbose, LogLevel::Debug};
            size_t len = strlen(message);
            if (message[len-1] == '\n')
                --len;
            TLSLogDomain.log(kLogLevels[level], "mbedTLS(%s): %.*s",
                             (role == Client ? "C" : "S"), int(len), message);
        });
    }

    TLSContext::~TLSContext() =default;


    void TLSContext::setRootCerts(slice certsData) {
        if(certsData) {
            _context->set_root_certs(string(certsData));
        } else {
            resetRootCertFinder();
        }
    }

    void TLSContext::setRootCerts(crypto::Cert *cert) {
        setRootCerts(cert->data());
    }

#ifdef ROOT_CERT_LOOKUP_AVAILABLE
    bool TLSContext::findSigningRootCert(const string &certStr, string &rootStr) {
        try {
            Retained<Cert> cert = new Cert(certStr);
            Retained<Cert> root = cert->findSigningRootCert();
            if (root)
                rootStr = string(root->dataOfChain());
            return true;
        } catch (const std::exception &x) {
            Warn("Unable to find a root cert: %s", x.what());
            return false;
        }
    }
#endif

    void TLSContext::requirePeerCert(bool require) {
        _context->require_peer_cert(tls_context::role_t(_role), require, false);
    }

    void TLSContext::allowOnlyCert(slice certData) {
        if(certData) {
            _context->allow_only_certificate(string(certData));
        } else {
            resetRootCertFinder();
        }
    }

    void TLSContext::allowOnlyCert(crypto::Cert *cert) {
        allowOnlyCert(cert->data());
    }

    void TLSContext::allowOnlySelfSigned(bool onlySelfSigned) {
        if(_onlySelfSigned == onlySelfSigned) {
            return;
        }
        
        _onlySelfSigned = onlySelfSigned;
        if(onlySelfSigned) {
            _context->set_root_cert_locator([](string certStr, string &rootStr) {
                // Don't return any CA certs, have those all fail
                return true;
            });
            
            _context->set_auth_callback([](const string& certData) {
                Retained<Cert> cert = new Cert(slice(certData));
                return cert->isSelfSigned();
            });
        } else {
            resetRootCertFinder();
        }
    }

    void TLSContext::setCertAuthCallback(std::function<bool(fleece::slice)> callback) {
        _context->set_auth_callback([=](const string &certData) {
            return callback(slice(certData));
        });
        
        resetRootCertFinder();
    }

    void TLSContext::setIdentity(crypto::Identity *id) {
        _context->set_identity(id->cert->context(), id->privateKey->context());
        _identity = id;
    }

    void TLSContext::setIdentity(slice certData, slice keyData) {
        _context->set_identity(string(certData), string(keyData));
    }

    void TLSContext::resetRootCertFinder() {
        #ifdef ROOT_CERT_LOOKUP_AVAILABLE
        _context->set_root_cert_locator([this](string certStr, string &rootStr) {
            return findSigningRootCert(certStr, rootStr);
        });
        #else
        _context->set_root_cert_locator(nullptr);
        #endif
    }


} }
