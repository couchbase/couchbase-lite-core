//
//  ListenerTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/24/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Listener.h"
#include "c4.hh"
#include "Certificate.hh"
#include "StringUtil.hh"


using namespace std;
using namespace fleece;
using namespace litecore::crypto;


class ListenerHarness {
public:

    ListenerHarness(C4ListenerConfig conf)
    :config(conf)
    { }


    ~ListenerHarness() {
        listener = nullptr; // stop listener
        gC4ExpectExceptions = false;
    }


    Cert* useServerIdentity(Identity *id, bool persistent) {
        C4Log("Using %s server TLS cert %s for this test",
              (persistent ? "persistent" : "temporary"),
              id->cert->subjectPublicKey()->digestString().c_str());
        serverIdentity = id;
        configCertData = id->cert->data();
        tlsConfig.certificate = configCertData;
        if (id->privateKey->isPrivateKeyDataAvailable()) {
            configKeyData = id->privateKey->privateKeyData();
            tlsConfig.privateKey = configKeyData;
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyData;
        } else {
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromCert;
        }
        config.tlsConfig = &tlsConfig;
        return id->cert;
    }


    Cert* useClientIdentity(Identity *id, bool persistent) {
        C4Log("Using %s client TLS cert %s for this test",
              (persistent ? "persistent" : "temporary"),
              id->cert->subjectPublicKey()->digestString().c_str());
        clientIdentity = id;
        configClientRootCertData = id->cert->data();
        tlsConfig.requireClientCerts = true;
        tlsConfig.rootClientCerts = configClientRootCertData;
        return id->cert;
    }


    Cert* useServerTLSWithTemporaryKey() {
        if (!sServerTemporaryIdentity)
            sServerTemporaryIdentity = createTemporaryIdentity("LiteCore Listener Test");
        return useServerIdentity(sServerTemporaryIdentity, false);
    }


    Cert* useClientTLSWithTemporaryKey() {
        if (!sClientTemporaryIdentity)
            sClientTemporaryIdentity = createTemporaryIdentity("LiteCore Client Test");
        return useClientIdentity(sClientTemporaryIdentity, false);
    }


    Identity* createTemporaryIdentity(const char *commonName) {
        C4Log("Generating temporary TLS key-pair and cert...")
        Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
        Cert::IssuerParameters issuerParams;
        issuerParams.validity_secs = 3600*24;
        auto cert = retained(new Cert(slice(litecore::format("CN=%s, O=Couchbase, OU=Mobile", commonName)),
                                      issuerParams, key));
        return new Identity(cert, key);
    }


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    Cert* useServerTLSWithPersistentKey() {
        C4Log("Using server TLS w/persistent key for this test");
        if (!sServerPersistentIdentity)
            sServerPersistentIdentity = createPersistentIdentity();
        return useServerIdentity(sServerPersistentIdentity, true);
    }


    Cert* useClientTLSWithPersistentKey() {
        if (!sClientPersistentIdentity)
            sClientPersistentIdentity = createPersistentIdentity();
        return useClientIdentity(sClientPersistentIdentity, true);
    }


    Identity* createPersistentIdentity() {
        C4Log("Generating persistent TLS key-pair and cert...")
        Retained<PersistentPrivateKey> key = PersistentPrivateKey::generateRSA(2048);
        Cert::IssuerParameters issuerParams;
        issuerParams.validity_secs = 3600*24;
        auto cert = retained(new Cert("CN=ListenerHarness, O=Couchbase, OU=Mobile"_sl, issuerParams, key));
        cert->makePersistent();
        return new Identity(cert, key);
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
    Retained<Identity> serverIdentity, clientIdentity;

private:
    static Retained<Identity> sServerTemporaryIdentity, sServerPersistentIdentity,
                              sClientTemporaryIdentity, sClientPersistentIdentity;

    c4::ref<C4Listener> listener;

    C4TLSConfig tlsConfig = { };
    alloc_slice configCertData, configKeyData, configClientRootCertData;
};

