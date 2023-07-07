//
// RESTClientTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ReplicatorAPITest.hh"
#include "HTTPLogic.hh"

using namespace fleece;
using namespace litecore::net;
using namespace std;


#define TEST_PROXIES 0

class RESTClientTest : public ReplicatorAPITest {
  public:
#if TEST_PROXIES
    static const int numberOfOptions = 2;
#else
    static const int numberOfOptions = 1;
#endif

    explicit RESTClientTest(int option) : ReplicatorAPITest() {
        if ( option == 0 ) {
            _sg.proxy = nullptr;
            fprintf(stderr, "        --- No proxy ---\n");
        } else if ( option == 1 ) {
            if ( _sg.proxy ) _sg.proxy = make_unique<ProxySpec>(ProxyType::HTTP, "localhost"_sl, uint16_t(8888));
            fprintf(stderr, "        --- HTTP proxy ---\n");
        }
    }
};

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTPS Request to public host") {
    c4address_fromURL("https://www.couchbase.com/"_sl, &(_sg.address), nullptr);
    _sg.remoteDBName   = ""_sl;
    alloc_slice result = _sg.sendRemoteRequest("GET", "");
}

/* REAL REST CLIENT TESTS

 The tests below are tagged [.SyncServer] to keep them from running during normal testing.
 Instead, they have to be invoked manually via the Catch command-line option `[.SyncServer]`.
 This is because they require that an external Sync Gateway process is running.

 The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
 overridden by setting environment vars REMOTE_HOST, REMOTE_PORT, REMOTE_DB.

 These tests require running an HTTP proxy on localhost. You can install tinyproxy and use
 the tinyproxy config script located in Replicator/tests/data.
 */

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Request", "[.SyncServer]") {
    alloc_slice result = _sg.sendRemoteRequest("GET", "");
    C4Log("Response: %.*s", SPLAT(result));
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Redirect", "[.SyncServer]") {
    // Lack of trailing "/" in path triggers a redirect from SG.
    alloc_slice result = _sg.sendRemoteRequest("GET", "/scratch");
    C4Log("Response: %.*s", SPLAT(result));
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Unauthorized", "[.SyncServer]") {
    _sg.remoteDBName   = kProtectedDBName;
    alloc_slice result = _sg.sendRemoteRequest("GET", "", nullslice, false, HTTPStatus::Unauthorized);
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Wrong Auth", "[.SyncServer]") {
    _sg.remoteDBName   = kProtectedDBName;
    _sg.authHeader     = HTTPLogic::basicAuth("pupshaw"_sl, "123456"_sl);
    alloc_slice result = _sg.sendRemoteRequest("GET", "", nullslice, false, HTTPStatus::Unauthorized);
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Authorized", "[.SyncServer]") {
    _sg.remoteDBName   = kProtectedDBName;
    _sg.authHeader     = HTTPLogic::basicAuth("pupshaw"_sl, "frank"_sl);
    alloc_slice result = _sg.sendRemoteRequest("GET", "", nullslice);
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Redirect Authorized", "[.SyncServer]") {
    _sg.remoteDBName   = kProtectedDBName;
    _sg.authHeader     = HTTPLogic::basicAuth("pupshaw"_sl, "frank"_sl);
    alloc_slice result = _sg.sendRemoteRequest("GET", "/seekrit", nullslice);
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Connection Refused", "[.SyncServer]") {
    //ExpectingExceptions x;
    _sg.address.hostname = C4STR("localhost");
    _sg.address.port     = 1;  // wrong port!
    HTTPStatus status;
    C4Error    error;
    _sg.sendRemoteRequest("GET", "", &status, &error);
    CHECK(error == (C4Error{POSIXDomain, ECONNREFUSED}));
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Unknown Host", "[.SyncServer]") {
    ExpectingExceptions x;
    _sg.address.hostname = C4STR("qux.ftaghn.miskatonic.edu");
    HTTPStatus status;
    C4Error    error;
    _sg.sendRemoteRequest("GET", "", &status, &error);
    CHECK(error == (C4Error{NetworkDomain, kC4NetErrUnknownHost}));
}

N_WAY_TEST_CASE_METHOD(RESTClientTest, "HTTP Timeout", "[.SyncServer]") {
    ExpectingExceptions x;
    _sg.address.hostname = C4STR("10.1.99.99");
    HTTPStatus status;
    C4Error    error;
    _sg.sendRemoteRequest("GET", "", &status, &error);
    CHECK(error == (C4Error{POSIXDomain, ETIMEDOUT}));
}
