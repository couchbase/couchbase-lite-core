//
// RESTClientTest.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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
#include "HTTPLogic.hh"

using namespace fleece;
using namespace litecore::net;


/* REAL REST CLIENT TESTS

 The tests below are tagged [.SyncServer] to keep them from running during normal testing.
 Instead, they have to be invoked manually via the Catch command-line option `[.SyncServer]`.
 This is because they require that an external Sync Gateway process is running.

 The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
 overridden by setting environment vars REMOTE_HOST, REMOTE_PORT, REMOTE_DB.

 These tests require running an HTTP proxy on localhost. You can install tinyproxy and use
 the tinyproxy config script located in Replicator/tests/data.
 */


class RESTClientTest : public ReplicatorAPITest {
public:
    static const int numberOfOptions = 3;

    RESTClientTest(int option)
    :ReplicatorAPITest()
    {
        if (!_proxy)
            _proxy = make_unique<ProxySpec>(ProxyType::HTTP, "http://localhost:8888"_sl);
        if (option == 0) {
            _proxy = nullptr;
            fprintf(stderr, "        --- No proxy ---\n");
        } else if (option == 1) {
            _proxy->type = ProxyType::HTTP;
            fprintf(stderr, "        --- HTTP proxy ---\n");
        } else if (option == 2) {
            _proxy->type = ProxyType::CONNECT;
            fprintf(stderr, "        --- CONNECT proxy ---\n");
        }
    }

};


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Request", "[.SyncServer][REST]") {
    alloc_slice result = sendRemoteRequest("GET", "");
    C4Log("Response: %.*s", SPLAT(result));
}


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Redirect", "[.SyncServer][REST]") {
    // Lack of trailing "/" in path triggers a redirect from SG.
    alloc_slice result = sendRemoteRequest("GET", "/scratch");
    C4Log("Response: %.*s", SPLAT(result));
}


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Unauthorized", "[.SyncServer][REST]") {
    _remoteDBName = kProtectedDBName;
    alloc_slice result = sendRemoteRequest("GET", "", nullslice, false, HTTPStatus::Unauthorized);
}


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Wrong Auth", "[.SyncServer][REST]") {
    _remoteDBName = kProtectedDBName;
    _authHeader = HTTPLogic::basicAuth("pupshaw"_sl, "123456"_sl);
    alloc_slice result = sendRemoteRequest("GET", "", nullslice, false, HTTPStatus::Unauthorized);
}


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Authorized", "[.SyncServer][REST]") {
    _remoteDBName = kProtectedDBName;
    _authHeader = HTTPLogic::basicAuth("pupshaw"_sl, "frank"_sl);
    alloc_slice result = sendRemoteRequest("GET", "", nullslice);
}


N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Redirect Authorized", "[.SyncServer][REST]") {
    _remoteDBName = kProtectedDBName;
    _authHeader = HTTPLogic::basicAuth("pupshaw"_sl, "frank"_sl);
    alloc_slice result = sendRemoteRequest("GET", "/seekrit", nullslice);
}
