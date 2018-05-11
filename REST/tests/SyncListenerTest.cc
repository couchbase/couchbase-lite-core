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

#include "ReplicatorAPITest.hh"
#include "c4Listener.h"
#include "c4.hh"

using namespace std;
using namespace fleece;
using namespace litecore::REST;


#ifdef COUCHBASE_ENTERPRISE

class C4SyncListenerTest : public ReplicatorAPITest {
public:

    C4SyncListenerTest() {
        createDB2();

        _address.scheme = C4STR("ws");
        _address.hostname = C4STR("localhost");
        _address.port = config.port;
        _remoteDBName = C4STR("db2");
    }

    void start() {
        if (listener)
            return;
        if ((c4listener_availableAPIs() & kC4SyncAPI) == 0)
            FAIL("Sync listener is unavailable in this build of LiteCore");
        C4Error err;
        listener = c4listener_start(&config, &err);
        REQUIRE(listener);
        REQUIRE(c4listener_shareDB(listener, C4STR("db2"), db2));
    }

    C4ListenerConfig config = {49849, kC4SyncAPI,
                               {}, false, false,    // REST-only stuff
                               true, true};         // allowPush, allowPull
    c4::ref<C4Listener> listener;
};


TEST_CASE_METHOD(C4SyncListenerTest, "P2P Sync", "[Push][C]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();
    replicate(kC4OneShot, kC4Disabled);
    CHECK(c4db_getDocumentCount(db2) == 100);
}

#endif
