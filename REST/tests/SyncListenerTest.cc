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

using namespace litecore::REST;


#ifdef COUCHBASE_ENTERPRISE

class C4SyncListenerTest : public ReplicatorAPITest, public ListenerHarness {
public:

    C4SyncListenerTest()
    :ReplicatorAPITest()
    ,ListenerHarness({49849, nullslice,
                      kC4SyncAPI,
                       nullptr,
                       {}, false, false,    // REST-only stuff
                       true, true})
    {
        createDB2();

        _address.scheme = kC4Replicator2Scheme;
        _address.hostname = C4STR("localhost");
        _address.port = config.port;
        _remoteDBName = C4STR("db2");
    }

    void run() {
        ReplicatorAPITest::importJSONLines(sFixturesDir + "names_100.json");
        share(db2, "db2"_sl);
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


#endif
