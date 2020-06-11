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
#include <algorithm>

using namespace litecore::REST;


#ifdef COUCHBASE_ENTERPRISE

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

    void run() {
        ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
        share(db2, "db2"_sl);
        _address.port = c4listener_getPort(listener());
        if (pinnedCert)
            _address.scheme = kC4Replicator2TLSScheme;
        replicate(kC4OneShot, kC4Disabled);
        CHECK(c4db_getDocumentCount(db2) == 100);
    }

};


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync", "[Push][Listener][C]") {
    run();
}


TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync pinned cert", "[Push][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    run();
}


TEST_CASE_METHOD(C4SyncListenerTest, "TLS P2P Sync client cert", "[Push][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    useClientTLSWithTemporaryKey();
    identityCert = clientIdentity.cert;
    identityKey  = clientIdentity.key;
    run();
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


TEST_CASE_METHOD(C4SyncListenerTest, "P2P ReadOnly Sync", "[Push][Listener][C]") {
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
    
    _callbackStatus.error = {WebSocketDomain, 10403};
    replicate(pushMode, pullMode, false);
    CHECK(c4db_getDocumentCount(db2) == 0);
}


#endif
