//
// SyncListenerTest.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "ListenerHarness.hh"
#include "ReplicatorAPITest.hh"
#include "Stopwatch.hh"
#include "Server.hh"
#include "NetworkInterfaces.hh"
#include "c4Replicator.h"
#include <algorithm>

using namespace litecore::REST;


#ifdef COUCHBASE_ENTERPRISE

static constexpr const char* kCAName = "CN=TrustMe Root CA, O=TrustMe Corp., C=US";

static constexpr const char* kSubjectName = "localhost, O=ExampleCorp, C=US, "
"pseudonym=3Jane";

class C4SyncListenerTest : public ReplicatorAPITest, public ListenerHarness {
public:

    C4SyncListenerTest()
    :ReplicatorAPITest()
    ,ListenerHarness({0, nullslice,
                      kC4SyncAPI,
                       nullptr,
                       nullptr, nullptr,
                       {}, false, false,    // REST-only stuff
                       true, true})
    {
        createDB2();

        _address.scheme = kC4Replicator2Scheme;
        _address.hostname = C4STR("localhost");
        _remoteDBName = C4STR("db2");
    }

    void run(bool expectSuccess = true) {
        ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
        share(db2, "db2"_sl);
        _address.port = c4listener_getPort(listener());
        if (pinnedCert)
            _address.scheme = kC4Replicator2TLSScheme;
        replicate(kC4OneShot, kC4Disabled, expectSuccess);
        if(expectSuccess) {
            CHECK(c4db_getDocumentCount(db2) == 100);
        }
    }

};


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync", "[Push][Listener][C]") {
    run();
}


TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync self-signed cert", "[Push][Listener][TLS][C]") {
    C4NetworkErrorCode expectedError = (C4NetworkErrorCode)0;
    
    // Pinned shouldn't differ betwen modes
    SECTION("Pinned, self-signed mode") {
        _onlySelfSigned = true;
        pinnedCert = useServerTLSWithTemporaryKey();
    }
    
    SECTION("Pinned, normal mode") {
        pinnedCert = useServerTLSWithTemporaryKey();
    }
    
    SECTION("Non-pinned, self-signed mode") {
        _address.scheme = kC4Replicator2TLSScheme;
        _onlySelfSigned = true;
        useServerTLSWithTemporaryKey();
    }
    
    SECTION("Non-pinned, normal mode") {
        _address.scheme = kC4Replicator2TLSScheme;
        expectedError = kC4NetErrTLSCertUnknownRoot;
        useServerTLSWithTemporaryKey();
    }
    
    run((int)expectedError == 0);
    CHECK(_callbackStatus.error.code == expectedError);
}


TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync non self-signed cert", "[Push][Listener][TLS][C]") {
    ::Identity caIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLS_CA, kCAName, nullptr, nullptr, true);
    ::Identity endIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kSubjectName, nullptr, &caIdentity, false);
    C4NetworkErrorCode expectedError = (C4NetworkErrorCode)0;
    slice expectedMessage {};
    
    
    // Pinned shouldn't differ betwen modes
    SECTION("Pinned, self-signed mode") {
        _onlySelfSigned = true;
        useServerIdentity(endIdentity);
        pinnedCert = c4cert_copyData(endIdentity.cert, false);
    }
    
    SECTION("Pinned, normal mode") {
        useServerIdentity(endIdentity);
        pinnedCert = c4cert_copyData(endIdentity.cert, false);
    }
    
    SECTION("Non-pinned, self-signed mode") {
        _address.scheme = kC4Replicator2TLSScheme;
        _onlySelfSigned = true;
        useServerIdentity(endIdentity);
        expectedError = kC4NetErrTLSCertUntrusted;
        expectedMessage = "Self-signed only mode is active, and a non self-signed certificate was received"_sl;
    }
    
    SECTION("Non-pinned, normal mode") {
        _address.scheme = kC4Replicator2TLSScheme;
        _customCaCert = c4cert_copyData(caIdentity.cert, false);
        useServerIdentity(endIdentity);
    }
    
    run((int)expectedError == 0);
    CHECK(_callbackStatus.error.code == expectedError);
    if(expectedMessage.buf) {
        alloc_slice gotMessage = c4error_getMessage(_callbackStatus.error);
        CHECK(gotMessage == expectedMessage);
    }
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync no CA bits", "[Push][Listener][TLS][C]") {
    // CA without the CA-bits sit
    Identity caIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kCAName, nullptr, nullptr, true);
    Identity endIdentity = CertHelper::createIdentity(false, kC4CertUsage_TLSServer, kSubjectName, nullptr, &caIdentity, false);
    
    _address.scheme = kC4Replicator2TLSScheme;
    useServerIdentity(endIdentity);
    run(false);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUnknownRoot);
}


TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync client cert", "[Push][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    useClientTLSWithTemporaryKey();
    identityCert = clientIdentity.cert;
    identityKey  = clientIdentity.key;
    run();
}

TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync Expired Cert", "[Push][Listener][TLS][C]") {
    C4Error error;
    Identity id;
    id.key = c4keypair_generate(kC4RSA, 2048, false, &error);
    REQUIRE(id.key);

    const C4CertNameComponent subjectName[4] = {
        {kC4Cert_CommonName,       "localhost"_sl},
        {kC4Cert_Organization,     "Couchbase"_sl},
        {kC4Cert_OrganizationUnit, "Mobile"_sl},
        {kC4Cert_EmailAddress,     nullslice} };
    c4::ref<C4Cert> csr = c4cert_createRequest(subjectName, 3,
                                               kC4CertUsage_TLSServer, id.key, &error);
    REQUIRE(csr);

    C4CertIssuerParameters issuerParams = kDefaultCertIssuerParameters;
    issuerParams.validityInSeconds = 0;
    issuerParams.isCA = false;
    id.cert = c4cert_signRequest(csr, &issuerParams, id.key, id.cert, &error);
    REQUIRE(id.cert);
    
    this_thread::sleep_for(1s);
    
    useServerIdentity(id);
    _onlySelfSigned = true;
    _address.scheme = kC4Replicator2TLSScheme;
    run(false);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertExpired);
}


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync pinned cert persistent key", "[Push][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithPersistentKey();
    run();
}
#endif


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync connection count", "[Listener][C]") {
    ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
    share(db2, "db2"_sl);
    _address.port = c4listener_getPort(listener());
    REQUIRE(_address.port != 0);
    if (pinnedCert)
        _address.scheme = kC4Replicator2TLSScheme;

    unsigned connections, activeConns;
    c4listener_getConnectionStatus(listener(), &connections, &activeConns);
    CHECK(connections == 0);
    CHECK(activeConns == 0);

    C4Error err;
    REQUIRE(startReplicator(kC4OneShot, kC4Disabled, &err));

    unsigned maxConnections = 0, maxActiveConns = 0;
    C4ReplicatorStatus status;
    while ((status = c4repl_getStatus(_repl)).level != kC4Stopped) {
        c4listener_getConnectionStatus(listener(), &connections, &activeConns);
        CHECK(activeConns <= connections);
        maxConnections = std::max(maxConnections, connections);
        maxActiveConns = std::max(maxActiveConns, activeConns);
        this_thread::sleep_for(chrono::milliseconds(1));
    }
    CHECK(maxConnections == 1);
    CHECK(maxActiveConns == 1);

    // It might take an instant for the counts to update:
    Stopwatch st;
    do {
        c4listener_getConnectionStatus(listener(), &connections, &activeConns);
        if (connections > 0) {
            C4Log("Waiting for connection count to reset to 0...");
            REQUIRE(st.elapsed() < 2.0);
            this_thread::sleep_for(10ms);
        }
    } while (connections > 0);
    CHECK(activeConns == 0);
}


TEST_CASE_METHOD(C4SyncListenerTest, "P2P ReadOnly Sync", "[Push][Pull][Listener][C]") {
    C4ReplicatorMode pushMode = kC4Disabled;
    C4ReplicatorMode pullMode = kC4Disabled;
    SECTION("Push") {
        config.allowPull = false;
        SECTION("Continuous") {
            pushMode = kC4Continuous;
        }
        
        SECTION("One-shot") {
            pushMode = kC4OneShot;
        }
    }
    
    SECTION("Pull") {
        config.allowPush = false;
        SECTION("Continuous") {
            pullMode = kC4Continuous;
        }
        
        SECTION("One-shot") {
            pullMode = kC4OneShot;
        }
    }
    
    ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
    share(db2, "db2"_sl);
    _address.port = c4listener_getPort(listener());
    if (pinnedCert)
        _address.scheme = kC4Replicator2TLSScheme;
    
    replicate(pushMode, pullMode, false);
    CHECK(c4db_getDocumentCount(db2) == 0);
}


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Server Addresses", "[Listener]") {
    fleece::Retained<Server> s(new Server());
    s->start(0);
    auto addresses = s->addresses();
    s->stop();
    
    for (const auto& addr : addresses) {
        auto parsed = litecore::net::IPAddress::parse(addr);
        if(!parsed) {
            // Probably the machine hostname
            continue;
        }
        
        if(parsed->isLinkLocal()) {
            // These addresses cannot be assigned via network interface
            // because they don't map to any given interface.  The fact
            // that an interface has them all is purely consequential.
            // The same address could just as easily be on another
            // interface.
            continue;
        }
        
        s->start(0, addr);
        auto innerAddresses = s->addresses();
        REQUIRE(innerAddresses.size() == 1);
        CHECK(innerAddresses[0] == addr);
        CHECK(s->port() != 0);
        s->stop();
    }
    
    for(const auto& interface : litecore::net::Interface::all()) {
        s->start(0, interface.name);
        auto innerAddresses = s->addresses();
        REQUIRE(innerAddresses.size() == 1);
        CHECK(innerAddresses[0] == (string)interface.primaryAddress());
        CHECK(s->port() != 0);
        s->stop();
    }
}


#endif
