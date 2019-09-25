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


    void useIdentity(Identity *id) {
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

    }


    Identity* useTLSWithTemporaryKey() {
        C4Log("Using TLS w/temporary key for this test");
        if (!sTemporaryIdentity) {
            C4Log("Generating TLS key-pair and cert...")
            Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
            Cert::IssuerParameters issuerParams;
            issuerParams.validity_secs = 3600*24;
            auto cert = retained(new Cert("CN=ListenerHarness, O=Couchbase, OU=Mobile", issuerParams, key));
            sTemporaryIdentity = new Identity(cert, key);
        }
        useIdentity(sTemporaryIdentity);
        return sTemporaryIdentity;
    }


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    Identity* useTLSWithPersistentKey() {
        C4Log("Using TLS w/persistent key for this test");
        if (!sPersistentIdentity) {
            Retained<PersistentPrivateKey> key = PersistentPrivateKey::generateRSA(2048);
            Cert::IssuerParameters issuerParams;
            issuerParams.validity_secs = 3600*24;
            auto cert = retained(new Cert("CN=ListenerHarness, O=Couchbase, OU=Mobile", issuerParams, key));
            cert->makePersistent();
            sPersistentIdentity = new Identity(cert, key);
        }
        useIdentity(sPersistentIdentity);
        return sPersistentIdentity;
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

private:
    static Retained<Identity> sTemporaryIdentity, sPersistentIdentity;

    c4::ref<C4Listener> listener;

    C4TLSConfig tlsConfig = { };
    alloc_slice configCertData, configKeyData;
};

