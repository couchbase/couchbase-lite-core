//
//  ListenerHarness.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/24/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "CertHelper.hh"

using namespace std;
using namespace fleece;


class ListenerHarness {
public:

    ListenerHarness(C4ListenerConfig conf)
    :config(conf)
    { }


    ~ListenerHarness() {
        listener = nullptr; // stop listener
        gC4ExpectExceptions = false;
    }


#ifdef COUCHBASE_ENTERPRISE

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


    alloc_slice useServerTLSWithTemporaryKey() {
        auto cert = useServerIdentity( CertHelper::instance().temporaryServerIdentity );
        return alloc_slice(c4cert_copyData(cert, false));
    }


    C4Cert* useClientTLSWithTemporaryKey() {
        return useClientIdentity( CertHelper::instance().temporaryClientIdentity );
    }


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    alloc_slice useServerTLSWithPersistentKey() {
        C4Log("Using server TLS w/persistent key for this test");
        auto cert = useServerIdentity( CertHelper::instance().persistentServerIdentity() );
        return alloc_slice(c4cert_copyData(cert, false));
    }


    C4Cert* useClientTLSWithPersistentKey() {
        return useClientIdentity(CertHelper::instance().persistentClientIdentity());
    }
#endif
#endif // COUCHBASE_ENTERPRISE

    void share(C4Database *dbToShare, slice name) {
        if (listener)
            return;
        auto missing = config.apis & ~c4listener_availableAPIs();
        if (missing)
            FAIL("Listener API " << missing << " is unavailable in this build");
        C4Error err;
        listener = c4listener_start(&config, &err);
        REQUIRE(listener);

        REQUIRE(c4listener_shareDB(listener, name, dbToShare, &err));
    }

    C4ListenerConfig config;
#ifdef COUCHBASE_ENTERPRISE
    Identity serverIdentity, clientIdentity;
#endif

    c4::ref<C4Listener> listener;

private:
    C4TLSConfig tlsConfig = { };
    alloc_slice configCertData, configKeyData, configClientRootCertData;
};

