//
//  ListenerHarness.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/24/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "CertHelper.hh"
#include "c4Listener.h"

using namespace fleece;

class ListenerHarness {
  public:
    explicit ListenerHarness(C4ListenerConfig conf) : config(conf) {}

    ~ListenerHarness() {
        _listener           = nullptr;  // stop listener
        gC4ExpectExceptions = false;
    }

    [[nodiscard]] C4Listener* listener() const { return _listener; }


#ifdef COUCHBASE_ENTERPRISE

    C4Cert* useServerIdentity(const Identity& id) {
        alloc_slice digest = c4keypair_publicKeyDigest(id.key);
        C4Log("Using %s server TLS cert %.*s for this test",
              (c4keypair_isPersistent(id.key) ? "persistent" : "temporary"), SPLAT(digest));
        serverIdentity         = id;
        _tlsConfig.certificate = id.cert;

        if ( id.key ) {
            _tlsConfig.key                      = id.key;
            _tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromKey;
        } else {
            _tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromCert;
        }
        config.tlsConfig = &_tlsConfig;
        return id.cert;
    }

    C4Cert* useClientIdentity(const Identity& id) {
        alloc_slice digest = c4keypair_publicKeyDigest(id.key);
        C4Log("Using %s client TLS cert %.*s for this test",
              (c4keypair_isPersistent(id.key) ? "persistent" : "temporary"), SPLAT(digest));
        clientIdentity = id;
        setListenerRootClientCerts(id.cert);
        return id.cert;
    }

    void setListenerRootClientCerts(C4Cert* certs) { _tlsConfig.rootClientCerts = certs; }

    alloc_slice useServerTLSWithTemporaryKey() {
        auto cert = useServerIdentity(_certHelper.temporaryServerIdentity);
        return {c4cert_copyData(cert, false)};
    }

    C4Cert* useClientTLSWithTemporaryKey() { return useClientIdentity(_certHelper.temporaryClientIdentity); }


#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    alloc_slice useServerTLSWithPersistentKey() {
        C4Log("Using server TLS w/persistent key for this test");
        auto cert = useServerIdentity(_certHelper.persistentServerIdentity());
        return {c4cert_copyData(cert, false)};
    }

    C4Cert* useClientTLSWithPersistentKey() { return useClientIdentity(_certHelper.persistentClientIdentity()); }


#    endif


    void setCertAuthCallback(C4ListenerCertAuthCallback callback, void* context) {
        _tlsConfig.certAuthCallback   = callback;
        _tlsConfig.tlsCallbackContext = context;
    }

#endif  // COUCHBASE_ENTERPRISE

    void share(C4Database* dbToShare, slice name) {
        if ( _listener ) return;
        auto missing = config.apis & ~c4listener_availableAPIs();
        if ( missing ) FAIL("Listener API " << missing << " is unavailable in this build");
        C4Error err;
        _listener = c4listener_start(&config, &err);
        REQUIRE(_listener);

        REQUIRE(c4listener_shareDB(_listener, name, dbToShare, &err));
    }

    void stop() { _listener = nullptr; }

  public:
    C4ListenerConfig config;
#ifdef COUCHBASE_ENTERPRISE
    Identity serverIdentity, clientIdentity;
#endif

  private:
    c4::ref<C4Listener> _listener;
    C4TLSConfig         _tlsConfig = {};
#ifdef COUCHBASE_ENTERPRISE
    CertHelper _certHelper;
#endif
};
