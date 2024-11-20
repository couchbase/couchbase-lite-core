//
// SyncListenerTest.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ListenerHarness.hh"
#include "ReplicatorAPITest.hh"
#include "c4Collection.h"
#include "Server.hh"
#include "NetworkInterfaces.hh"
#include "c4Replicator.h"
#include <algorithm>

using namespace litecore::REST;
using namespace std;


#ifdef COUCHBASE_ENTERPRISE

static constexpr const char* kCAName = "TrustMe Root CA";

static constexpr const char* kSubjectName = "localhost";

class C4SyncListenerTest
    : public ReplicatorAPITest
    , public ListenerHarness {
  public:
    C4SyncListenerTest() : ReplicatorAPITest(), ListenerHarness(kConfig) {
        createDB2();

        _sg.address.scheme   = kC4Replicator2Scheme;
        _sg.address.hostname = C4STR("localhost");
        _sg.remoteDBName     = C4STR("db2");
    }

    static constexpr C4ListenerConfig kConfig = {
            .allowPush = true,
            .allowPull = true,
    };

    void run(bool expectSuccess = true) {
        ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
        share(db2, "db2"_sl);
        _sg.address.port = c4listener_getPort(listener());
        if ( _sg.pinnedCert ) _sg.address.scheme = kC4Replicator2TLSScheme;
        replicate(kC4OneShot, kC4Disabled, expectSuccess);
        if ( expectSuccess ) {
            auto defaultColl = getCollection(db2, kC4DefaultCollectionSpec);
            CHECK(c4coll_getDocumentCount(defaultColl) == 100);
        }
    }
};

TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync", "[Push][Listener][C]") { run(); }

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync self-signed cert", "[Push][Listener][TLS][C]") {
    C4NetworkErrorCode expectedError = (C4NetworkErrorCode)0;

    // Pinned shouldn't differ betwen modes
    SECTION("Pinned, self-signed mode") {
        _onlySelfSigned = true;
        _sg.pinnedCert  = useServerTLSWithTemporaryKey();
    }

    SECTION("Pinned, normal mode") { _sg.pinnedCert = useServerTLSWithTemporaryKey(); }

    SECTION("Non-pinned, self-signed mode") {
        _sg.address.scheme = kC4Replicator2TLSScheme;
        _onlySelfSigned    = true;
        (void)useServerTLSWithTemporaryKey();
    }

    SECTION("Non-pinned, normal mode") {
        _sg.address.scheme = kC4Replicator2TLSScheme;
        expectedError      = kC4NetErrTLSCertUnknownRoot;
        (void)useServerTLSWithTemporaryKey();
    }

    run((int)expectedError == 0);
    CHECK(_callbackStatus.error.code == expectedError);
    REQUIRE(_sg.remoteCert);
    alloc_slice receivedCert = c4cert_copyData(_sg.remoteCert, false);
    alloc_slice expectedCert = c4cert_copyData(serverIdentity.cert, false);
    CHECK(receivedCert == expectedCert);
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync non self-signed cert", "[Push][Listener][TLS][C]") {
    ::Identity caIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLS_CA, kCAName, nullptr, nullptr, true);
    ::Identity endIdentity =
            CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kSubjectName, nullptr, &caIdentity, false);
    C4NetworkErrorCode expectedError = (C4NetworkErrorCode)0;
    slice              expectedMessage{};

    // Pinned shouldn't differ betwen modes
    SECTION("Pinned, self-signed mode") {
        _onlySelfSigned = true;
        useServerIdentity(endIdentity);
        _sg.pinnedCert = c4cert_copyData(endIdentity.cert, false);
    }

    SECTION("Pinned, normal mode") {
        useServerIdentity(endIdentity);
        _sg.pinnedCert = c4cert_copyData(endIdentity.cert, false);
    }

    SECTION("Non-pinned, self-signed mode") {
        _sg.address.scheme = kC4Replicator2TLSScheme;
        _onlySelfSigned    = true;
        useServerIdentity(endIdentity);
        expectedError   = kC4NetErrTLSCertUntrusted;
        expectedMessage = "Self-signed only mode is active, and a non self-signed certificate was received"_sl;
    }

    SECTION("Non-pinned, normal mode") {
        _sg.address.scheme = kC4Replicator2TLSScheme;
        _customCaCert      = c4cert_copyData(caIdentity.cert, false);
        useServerIdentity(endIdentity);
    }

    run((int)expectedError == 0);
    CHECK(_callbackStatus.error.code == expectedError);
    REQUIRE(_sg.remoteCert);
    alloc_slice receivedCert = c4cert_copyData(_sg.remoteCert, false);
    alloc_slice expectedCert = c4cert_copyData(endIdentity.cert, false);
    CHECK(receivedCert == expectedCert);
    if ( expectedMessage.buf ) {
        alloc_slice gotMessage = c4error_getMessage(_callbackStatus.error);
        CHECK(gotMessage == expectedMessage);
    }
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync no CA bits", "[Push][Listener][TLS][C]") {
    // CA without the CA-bits sit
    Identity caIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kCAName, nullptr, nullptr, true);
    Identity endIdentity =
            CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kSubjectName, nullptr, &caIdentity, false);

    _sg.address.scheme = kC4Replicator2TLSScheme;
    useServerIdentity(endIdentity);
    run(false);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUnknownRoot);
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync client cert", "[Push][Listener][TLS][C]") {
    _sg.pinnedCert = useServerTLSWithTemporaryKey();
    useClientTLSWithTemporaryKey();
    _sg.identityCert = clientIdentity.cert;
    _sg.identityKey  = clientIdentity.key;
    run();
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync Expired Cert", "[Push][Listener][TLS][C]") {
    Identity id;
    id.key = c4keypair_generate(kC4RSA, 2048, false, ERROR_INFO());
    REQUIRE(id.key);

    const C4CertNameComponent subjectName[4] = {{kC4Cert_CommonName, "localhost"_sl},
                                                {kC4Cert_Organization, "Couchbase"_sl},
                                                {kC4Cert_OrganizationUnit, "Mobile"_sl},
                                                {kC4Cert_EmailAddress, nullslice}};
    c4::ref<C4Cert>           csr = c4cert_createRequest(subjectName, 3, kC4CertUsage_TLSServer, id.key, ERROR_INFO());
    REQUIRE(csr);

    C4CertIssuerParameters issuerParams = kDefaultCertIssuerParameters;
    issuerParams.validityInSeconds      = 0;
    issuerParams.isCA                   = false;
    id.cert                             = c4cert_signRequest(csr, &issuerParams, id.key, id.cert, ERROR_INFO());
    REQUIRE(id.cert);

    this_thread::sleep_for(1s);

    useServerIdentity(id);
    _onlySelfSigned    = true;
    _sg.address.scheme = kC4Replicator2TLSScheme;
    run(false);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertExpired);
}


#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync pinned cert persistent key", "[Push][Listener][TLS][C]") {
    {
        ExpectingExceptions x;
        _sg.pinnedCert = useServerTLSWithPersistentKey();
    }
    run();
}
#    endif


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync connection count", "[Listener][C]") {
    ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
    share(db2, "db2"_sl);
    _sg.address.port = c4listener_getPort(listener());
    REQUIRE(_sg.address.port != 0);
    if ( _sg.pinnedCert ) _sg.address.scheme = kC4Replicator2TLSScheme;

    unsigned connections, activeConns;
    c4listener_getConnectionStatus(listener(), &connections, &activeConns);
    CHECK(connections == 0);
    CHECK(activeConns == 0);

    REQUIRE(startReplicator(kC4OneShot, kC4Disabled, WITH_ERROR()));

    // Count the maximum number of connections while the replicator is running:
    unsigned           maxConnections = 0, maxActiveConns = 0;
    C4ReplicatorStatus status;
    while ( (status = c4repl_getStatus(_repl)).level != kC4Stopped ) {
        c4listener_getConnectionStatus(listener(), &connections, &activeConns);
        CHECK(activeConns <= connections);
        maxConnections = std::max(maxConnections, connections);
        maxActiveConns = std::max(maxActiveConns, activeConns);
        this_thread::sleep_for(1ms);
    }
    CHECK(maxConnections == 1);
    CHECK(maxActiveConns == 1);

    // It might take an instant for the counts to update:
    (void)WaitUntil(2s, [&] {
        c4listener_getConnectionStatus(listener(), &connections, &activeConns);
        return connections == 0;
    });
    CHECK(connections == 0);
    CHECK(activeConns == 0);
}

TEST_CASE_METHOD(C4SyncListenerTest, "P2P ReadOnly Sync", "[Push][Pull][Listener][C]") {
    // This method tests disabling push or pull in the listener.
    // All these replications are expected to fail because the listener prevents them.
    C4ReplicatorMode pushMode = kC4Disabled;
    C4ReplicatorMode pullMode = kC4Disabled;
    SECTION("Push") {
        config.allowPull = false;
        SECTION("Continuous") { pushMode = kC4Continuous; }

        SECTION("One-shot") { pushMode = kC4OneShot; }
    }

    SECTION("Pull") {
        config.allowPush = false;
        SECTION("Continuous") { pullMode = kC4Continuous; }

        SECTION("One-shot") { pullMode = kC4OneShot; }
    }

    ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
    share(db2, "db2"_sl);
    _sg.address.port = c4listener_getPort(listener());
    if ( _sg.pinnedCert ) _sg.address.scheme = kC4Replicator2TLSScheme;

    replicate(pushMode, pullMode, false);
    auto defaultColl = getCollection(db2, kC4DefaultCollectionSpec);
    CHECK(c4coll_getDocumentCount(defaultColl) == 0);
}

TEST_CASE_METHOD(C4SyncListenerTest, "P2P Server Addresses", "[Listener]") {
    fleece::Retained<Server> s(new Server());
    s->start(0);
    auto addresses = s->addresses();
    s->stop();

    for ( const auto& addr : addresses ) {
        auto parsed = litecore::net::IPAddress::parse(addr);
        if ( !parsed ) {
            // Probably the machine hostname
            continue;
        }

        if ( parsed->isLinkLocal() ) {
            // These addresses cannot be assigned via network interface
            // because they don't map to any given interface.  The fact
            // that an interface has them all is purely consequential.
            // The same address could just as easily be on another
            // interface.
            continue;
        }

        C4Log("  >>> Starting server at %s", addr.c_str());
        s->start(0, addr);
        auto innerAddresses = s->addresses();
        REQUIRE(innerAddresses.size() == 1);
        CHECK(innerAddresses[0] == addr);
        CHECK(s->port() != 0);
        C4Log("  <<< Stopping server %s on port %d", addr.c_str(), s->port());
        s->stop();
    }

    for ( const auto& interface : litecore::net::Interface::all() ) {
        C4Log("  >>> Starting server at %s", interface.name.c_str());
        s->start(0, interface.name);
        auto innerAddresses = s->addresses();
        REQUIRE(innerAddresses.size() == 1);
        CHECK(innerAddresses[0] == (string)interface.primaryAddress());
        CHECK(s->port() != 0);
        C4Log("  <<< Stopping server at %s on port %d", interface.name.c_str(), s->port());
        s->stop();
    }
}

TEST_CASE_METHOD(C4SyncListenerTest, "Listener stops replicators", "[Listener]") {
    ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
    share(db2, "db2"_sl);
    _sg.address.port = c4listener_getPort(listener());
    REQUIRE(startReplicator(kC4Continuous, kC4Continuous, WITH_ERROR()));
    waitForStatus(kC4Idle);
    C4Log("  >>> Replicator is idle; stopping");
    stop();
    waitForStatus(kC4Stopped);
}
#endif
