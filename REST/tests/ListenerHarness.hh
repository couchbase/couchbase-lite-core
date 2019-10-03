//
//  ListenerHarness.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/24/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Listener.h"
#include "c4Certificate.h"
#include "c4Private.h"
#include "c4Test.hh"

#include "c4.hh"
#include "StringUtil.hh"


using namespace std;
using namespace fleece;


struct Identity {
    c4::ref<C4Cert> cert = nullptr;
    c4::ref<C4KeyPair> key = nullptr;
};


class ListenerHarness {
public:

    ListenerHarness(C4ListenerConfig conf)
    :config(conf)
    { }


    ~ListenerHarness() {
        listener = nullptr; // stop listener
        gC4ExpectExceptions = false;
    }


    C4Cert* useServerIdentity(const Identity &id) {
        alloc_slice digest = c4keypair_publicKeyDigest(id.key);
        C4Log("Using %s server TLS cert %.*s for this test",
              (c4keypair_isPersistent(id.key) ? "persistent" : "temporary"), SPLAT(digest));
        serverIdentity = id;

        configCertData = alloc_slice(c4cert_copyChainData(id.cert));
        tlsConfig.certificate = configCertData;

        configKeyData = alloc_slice(c4keypair_privateKeyData(id.key));
        if (configKeyData) {
            tlsConfig.privateKey = configKeyData;
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyData;
        } else {
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromCert;
        }
        config.tlsConfig = &tlsConfig;
        return id.cert;
    }


    C4Cert* useClientIdentity(const Identity &id) {
        alloc_slice digest = c4keypair_publicKeyDigest(id.key);
        C4Log("Using %s client TLS cert %.*s for this test",
              (c4keypair_isPersistent(id.key) ? "persistent" : "temporary"), SPLAT(digest));
        clientIdentity = id;
        setListenerRootClientCerts(id.cert);
        return id.cert;
    }


    void setListenerRootClientCerts(C4Cert *certs) {
        configClientRootCertData = alloc_slice(c4cert_copyChainData(certs));
        tlsConfig.requireClientCerts = true;
        tlsConfig.rootClientCerts = configClientRootCertData;
    }


    C4Cert* useServerTLSWithTemporaryKey() {
        if (!sServerTemporaryIdentity.cert)
            sServerTemporaryIdentity = createIdentity(false, kC4CertUsage_TLSServer, "LiteCore Listener Test");
        return useServerIdentity(sServerTemporaryIdentity);
    }


    C4Cert* useClientTLSWithTemporaryKey() {
        if (!sClientTemporaryIdentity.cert)
            sClientTemporaryIdentity = createIdentity(false, kC4CertUsage_TLSClient, "LiteCore Client Test");
        return useClientIdentity(sClientTemporaryIdentity);
    }


    Identity createIdentity(bool persistent, C4CertUsage usage, const char *commonName,
                            const Identity *signingIdentity =nullptr, bool isCA =false) {
        C4Log("Generating %s TLS key-pair and cert...", (persistent ? "persistent" : "temporary"))
        C4Error error;
        Identity id;
        id.key = c4keypair_generate(kC4RSA, 2048, persistent, &error);
        REQUIRE(id.key);

        const C4CertNameComponent subjectName[3] = {
            {kC4Cert_CommonName,       slice(commonName)},
            {kC4Cert_Organization,     "Couchbase"_sl},
            {kC4Cert_OrganizationUnit, "Mobile"_sl} };
        c4::ref<C4Cert> csr = c4cert_createRequest(subjectName, 3, usage, id.key, &error);
        REQUIRE(csr);

        if (!signingIdentity)
            signingIdentity = &id;

        C4CertIssuerParameters issuerParams = kDefaultCertIssuerParameters;
        issuerParams.validityInSeconds = 3600;
        issuerParams.isCA = isCA;
        id.cert = c4cert_signRequest(csr, &issuerParams, signingIdentity->key, signingIdentity->cert, &error);
        REQUIRE(id.cert);
        return id;
    }


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    C4Cert* useServerTLSWithPersistentKey() {
        C4Log("Using server TLS w/persistent key for this test");
        if (!sServerPersistentIdentity)
            sServerPersistentIdentity = createIdentity(true, kC4CertUsage_TLSServer, "ListenerHarness");
        return useServerIdentity(sServerPersistentIdentity);
    }


    C4Cert* useClientTLSWithPersistentKey() {
        if (!sClientPersistentIdentity)
            sClientPersistentIdentity = createIdentity(true, kC4CertUsage_TLSClient, "ListenerHarness");
        return useClientIdentity(sClientPersistentIdentity);
    }
#endif


    void share(C4Database *dbToShare, slice name) {
        if (listener)
            return;
        auto missing = config.apis & ~c4listener_availableAPIs();
        if (missing)
            FAIL("Listener API " << missing << " is unavailable in this build");
        C4Error err;
        listener = c4listener_start(&config, &err);
        REQUIRE(listener);

        REQUIRE(c4listener_shareDB(listener, name, dbToShare));
    }

    C4ListenerConfig config;
    Identity serverIdentity, clientIdentity;

private:
    static Identity sServerTemporaryIdentity, sServerPersistentIdentity,
                    sClientTemporaryIdentity, sClientPersistentIdentity;

    c4::ref<C4Listener> listener;

    C4TLSConfig tlsConfig = { };
    alloc_slice configCertData, configKeyData, configClientRootCertData;
};

