//
// CertHelper.hh
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
#include "c4Certificate.h"
#include "c4Certificate.hh"
#include "PublicKey.hh"  // just for PERSISTENT_PRIVATE_KEY_AVAILABLE
#include "StringUtil.hh"
#include "c4Test.hh"
#include "c4CppUtils.hh"
using namespace fleece;

struct Identity {
    c4::ref<C4Cert>    cert = nullptr;
    c4::ref<C4KeyPair> key  = nullptr;
};


#ifdef COUCHBASE_ENTERPRISE

class CertHelper {
  public:
    CertHelper()
        : temporaryServerIdentity(createIdentity(false, kC4CertUsage_TLSServer, "LiteCore Listener Test"))
        , temporaryClientIdentity(createIdentity(false, kC4CertUsage_TLSClient, "LiteCore Client Test")) {}

#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    ~CertHelper() {
        if ( _serverPersistentIdentity.key ) {
            (void)c4keypair_removePersistent(_serverPersistentIdentity.key.get(), nullptr);
        }
        if ( _clientPersistentIdentity.key ) {
            (void)c4keypair_removePersistent(_clientPersistentIdentity.key.get(), nullptr);
        }
    }
#    endif

    Identity const temporaryServerIdentity;

    Identity const temporaryClientIdentity;


#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    const Identity& persistentServerIdentity() {
        C4Log("Using server TLS w/persistent key for this test");
        if ( !_serverPersistentIdentity.cert )
            _serverPersistentIdentity = createIdentity(true, kC4CertUsage_TLSServer, "ListenerHarness");
        return _serverPersistentIdentity;
    }

    const Identity& persistentClientIdentity() {
        if ( !_clientPersistentIdentity.cert )
            _clientPersistentIdentity = createIdentity(true, kC4CertUsage_TLSClient, "ListenerHarness");
        return _clientPersistentIdentity;
    }
#    endif


    // Read cert & private key from files
    static Identity readIdentity(const std::string& certPath, const std::string& keyPath,
                                 const std::string& keyPassword) {
        Identity id{c4cert_fromData(C4Test::readFile(certPath), nullptr),
                    c4keypair_fromPrivateKeyData(C4Test::readFile(keyPath), slice(keyPassword), nullptr)};
        REQUIRE(id.cert);
        REQUIRE(id.key);
        return id;
    }

    static Identity createIdentity(bool persistent, C4CertUsage usage, const char* commonName,
                                   const char* email = nullptr, const Identity* signingIdentity = nullptr,
                                   bool isCA = false) {
        C4Log("Generating %s TLS key-pair and cert...", (persistent ? "persistent" : "temporary"));
        C4Error  error;
        Identity id;
        id.key = c4keypair_generate(kC4RSA, 2048, persistent, &error);
        REQUIRE(id.key);

        const C4CertNameComponent subjectName[4] = {{kC4Cert_CommonName, slice(commonName)},
                                                    {kC4Cert_Organization, "Couchbase"_sl},
                                                    {kC4Cert_OrganizationUnit, "Mobile"_sl},
                                                    {kC4Cert_EmailAddress, slice(email)}};
        c4::ref<C4Cert>           csr = c4cert_createRequest(subjectName, (email ? 4 : 3), usage, id.key, &error);
        REQUIRE(csr);

        if ( !signingIdentity ) signingIdentity = &id;

        C4CertIssuerParameters issuerParams = kDefaultCertIssuerParameters;
        issuerParams.validityInSeconds      = 3600;
        issuerParams.isCA                   = isCA;
        id.cert = c4cert_signRequest(csr, &issuerParams, signingIdentity->key, signingIdentity->cert, &error);
        REQUIRE(id.cert);
        return id;
    }

  private:
    Identity _serverPersistentIdentity, _clientPersistentIdentity, _ca;
};

#endif  // COUCHBASE_ENTERPRISE
