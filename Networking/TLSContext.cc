//
// TLSContext.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "TLSContext.hh"

#include "c4Certificate.hh"
#include "c4ListenerTypes.h"
#include "c4ReplicatorTypes.h"
#include "Certificate.hh"
#include "Error.hh"
#include "Logging.hh"
#include "fleece/Fleece.hh"
#include "sockpp/mbedtls_context.h"
#include <string>

using namespace std;
using namespace sockpp;
using namespace fleece;

namespace litecore::net {
    using namespace crypto;

    Retained<TLSContext> TLSContext::fromReplicatorOptions(Dict options, PrivateKey* externalKey,
                                                           const CertAuthCallback& certAuthCallback) {
        if ( !options ) return nullptr;
        Dict  authDict       = options[kC4ReplicatorOptionAuthentication].asDict();
        slice authType       = authDict[kC4ReplicatorAuthType].asString();
        slice rootCerts      = options[kC4ReplicatorOptionRootCerts].asData();
        slice pinnedCert     = options[kC4ReplicatorOptionPinnedServerCert].asData();
        bool  selfSignedOnly = options[kC4ReplicatorOptionOnlySelfSignedServerCert].asBool();
        if ( rootCerts || pinnedCert || selfSignedOnly || certAuthCallback
             || authType == slice(kC4AuthTypeClientCert) ) {
            if ( selfSignedOnly && rootCerts )
                error::_throw(error::InvalidParameter, "Cannot specify root certs in self signed mode");

            auto tlsContext = make_retained<TLSContext>(Client);
            tlsContext->allowOnlySelfSigned(selfSignedOnly);
            if ( rootCerts ) tlsContext->setRootCerts(rootCerts);
            if ( pinnedCert ) tlsContext->allowOnlyCert(pinnedCert);
            if ( certAuthCallback ) tlsContext->setCertAuthCallback(certAuthCallback);

            if ( authType == slice(kC4AuthTypeClientCert) ) {
                slice certData = authDict[kC4ReplicatorAuthClientCert].asData();
                if ( !certData )
                    error::_throw(error::InvalidParameter, "Missing TLS client cert in C4Replicator config");
                if ( externalKey ) {
                    Retained<Cert> cert = make_retained<Cert>(certData);
                    tlsContext->setIdentity(new Identity(cert, externalKey));
                } else if ( slice keyData = authDict[kC4ReplicatorAuthClientCertKey].asData(); keyData ) {
                    tlsContext->setIdentity(certData, keyData);
                } else {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
                    Retained<Cert>       cert = new Cert(certData);
                    Retained<PrivateKey> key  = cert->loadPrivateKey();
                    if ( !key ) error::_throw(error::CryptoError, "Couldn't find private key for identity cert");
                    tlsContext->setIdentity(new Identity(cert, key));
#else
                    error::_throw(error::InvalidParameter, "Missing TLS private key in C4Replicator config");
#endif
                }
            }
            return tlsContext;

        } else {
            return nullptr;
        }
    }

#ifdef COUCHBASE_ENTERPRISE
    Ref<TLSContext> TLSContext::fromListenerOptions(const C4TLSConfig* tlsConfig, C4Listener* c4Listener) {
        Assert(tlsConfig->certificate);
        Retained<Cert>       cert = tlsConfig->certificate->assertSignedCert();
        Retained<PrivateKey> privateKey;
        switch ( tlsConfig->privateKeyRepresentation ) {
            case kC4PrivateKeyFromKey:
                privateKey = tlsConfig->key->getPrivateKey();
                break;
            case kC4PrivateKeyFromCert:
#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
                privateKey = cert->loadPrivateKey();
                if ( !privateKey )
                    error::_throw(error::CryptoError,
                                  "No persistent private key found matching certificate public key");
                break;
#    else
                error::_throw(error::Unimplemented, "kC4PrivateKeyFromCert not implemented");
#    endif
        }

        auto tlsContext = make_retained<TLSContext>(Server);
        tlsContext->setIdentity(new Identity(cert, privateKey));
        if ( tlsConfig->requireClientCerts ) tlsContext->requirePeerCert(true);
        if ( tlsConfig->rootClientCerts ) tlsContext->setRootCerts(tlsConfig->rootClientCerts->assertSignedCert());

        if ( auto callback = tlsConfig->certAuthCallback ) {
            auto context = tlsConfig->tlsCallbackContext;
            tlsContext->setCertAuthCallback([callback, c4Listener, context](slice certData) {
                return callback(c4Listener, certData, context);
            });
        }

        return tlsContext;
    }
#endif

    TLSContext::TLSContext(role_t role)
        : _context(new mbedtls_context(role == Client ? tls_context::CLIENT : tls_context::SERVER)), _role(role) {
#ifdef ROOT_CERT_LOOKUP_AVAILABLE
        _context->set_root_cert_locator(
                [](const string& certStr, string& rootStr) { return findSigningRootCert(certStr, rootStr); });
#endif

        // Set up mbedTLS logging. mbedTLS log levels are numbered:
        //   0 No debug
        //   1 Error
        //   2 State change
        //   3 Informational
        //   4 Verbose
        int mbedLogLevel = 1;
        if ( auto logLevel = TLSLogDomain.effectiveLevel(); logLevel == LogLevel::Verbose ) mbedLogLevel = 2;
        else if ( logLevel == LogLevel::Debug )
            mbedLogLevel = 4;
        _context->set_logger(mbedLogLevel, [=](int level, const char* filename, int line, const char* message) {
            // mbedTLS logging callback:
            static const LogLevel kLogLevels[] = {LogLevel::Info, LogLevel::Info, LogLevel::Verbose, LogLevel::Verbose,
                                                  LogLevel::Debug};
            string_view           str(message);
            if ( str.ends_with('\n') ) str = str.substr(0, str.size() - 1);
            TLSLogDomain.log(kLogLevels[level], "mbedTLS(%s): %.*s", (role == Client ? "C" : "S"), int(str.size()),
                             str.data());
        });
    }

    TLSContext::~TLSContext() = default;

    void TLSContext::setRootCerts(slice certsData) {
        if ( certsData ) {
            _context->set_root_certs(string(certsData));
        } else {
            resetRootCertFinder();
        }
    }

    void TLSContext::setRootCerts(crypto::Cert* cert) { setRootCerts(cert->data()); }

#ifdef ROOT_CERT_LOOKUP_AVAILABLE
    bool TLSContext::findSigningRootCert(const string& certStr, string& rootStr) {
        try {
            Retained<Cert> cert = new Cert(certStr);
            Retained<Cert> root = cert->findSigningRootCert();
            if ( root ) rootStr = string(root->dataOfChain());
            return true;
        } catch ( const std::exception& x ) {
            Warn("Unable to find a root cert: %s", x.what());
            return false;
        }
    }
#endif

    void TLSContext::requirePeerCert(bool require) {
        _context->require_peer_cert(tls_context::role_t(_role), require, false);
    }

    void TLSContext::allowOnlyCert(slice certData) {
        if ( certData ) {
            _context->allow_only_certificate(string(certData));
            _onlyOneCert = true;
        } else {
            resetRootCertFinder();
            _onlyOneCert = false;
        }
    }

    void TLSContext::allowOnlyCert(crypto::Cert* cert) { allowOnlyCert(cert->data()); }

    void TLSContext::allowOnlySelfSigned(bool onlySelfSigned) {
        if ( _onlySelfSigned == onlySelfSigned ) { return; }

        _onlySelfSigned = onlySelfSigned;
        if ( onlySelfSigned ) {
            _context->set_root_cert_locator([](const string& certStr, string& rootStr) {
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

    void TLSContext::setCertAuthCallback(const std::function<bool(fleece::slice)>& callback) {
        _context->set_auth_callback([=](const string& certData) { return callback(slice(certData)); });

        resetRootCertFinder();
    }

    void TLSContext::setIdentity(crypto::Identity* id) {
        _context->set_identity(id->cert->context(), id->privateKey->context());
        _identity = id;
    }

    void TLSContext::setIdentity(slice certData, slice keyData) {
        _context->set_identity(string(certData), string(keyData));
    }

    unique_ptr<tls_socket> TLSContext::wrapSocket(unique_ptr<stream_socket> socket, const string& peer_name) {
        return _context->wrap_socket(std::move(socket), tls_context::role_t(_role), peer_name);
    }

    void TLSContext::resetRootCertFinder() {
#ifdef ROOT_CERT_LOOKUP_AVAILABLE
        _context->set_root_cert_locator(
                [](const string& certStr, string& rootStr) { return findSigningRootCert(certStr, rootStr); });
#else
        _context->set_root_cert_locator(nullptr);
#endif
    }


}  // namespace litecore::net
