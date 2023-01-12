//
//  ReplicatorAPITest.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/10/17.
//  Copyright 2017-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "ReplicatorAPITest.hh"
#include "c4Document+Fleece.h"
#include "c4Collection.h"
#include "c4ReplicatorHelpers.hh"
#include "StringUtil.hh"
#include "c4Socket.h"
//#include "c4Socket+Internal.hh"
#include "c4Socket.hh"
#include "c4Internal.hh"
#include "fleece/Fleece.hh"
#include "c4ReplicatorImpl.hh"

using namespace fleece;
using namespace std;
using namespace litecore;

constexpr const C4Address ReplicatorAPITest::kDefaultAddress;
constexpr const C4String ReplicatorAPITest::kScratchDBName, ReplicatorAPITest::kITunesDBName,
                         ReplicatorAPITest::kWikipedia1kDBName,
                         ReplicatorAPITest::kProtectedDBName,
                         ReplicatorAPITest::kImagesDBName;
std::once_flag           ReplicatorAPITest::once;

TEST_CASE("URL Parsing", "[C]][Replicator]") {
    C4Address address;
    C4String dbName;

    REQUIRE(c4address_fromURL("ws://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("ws://localhost/dbname"_sl, &address, NULL));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/dbname"_sl);

    REQUIRE(c4address_fromURL("ws://localhost/"_sl, &address, NULL));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("http://192.168.7.20:59849/"_sl, &address, NULL));
    CHECK(address.scheme == "http"_sl);
    CHECK(address.hostname == "192.168.7.20"_sl);
    CHECK(address.port == 59849);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("http://[fe80:2f::3c]:59849/"_sl, &address, NULL));
    CHECK(address.scheme == "http"_sl);
    CHECK(address.hostname == "fe80:2f::3c"_sl);
    CHECK(address.port == 59849);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("file:///path/to/dbname/"_sl, &address, nullptr));
    CHECK(address.scheme == "file"_sl);
    CHECK(address.hostname == ""_sl);
    CHECK(address.port == 0);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname/"_sl, &address, NULL));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/d"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost/p/d/"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost//p//d/"_sl, &address, &dbName));

    REQUIRE(!c4address_fromURL("ws://example.com/db@name"_sl, &address, &dbName));
    CHECK(dbName == "db@name"_sl);

    // The following URLs should all be rejected:
    ExpectingExceptions x;
    CHECK(!c4address_fromURL(""_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws:"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws:/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("*://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/null"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/nu:ll"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:-1/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:666666/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:x/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:/foo"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost/B^dn^m*"_sl, &address, &dbName));

    CHECK(!c4address_fromURL("ws://snej@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej@example.com:8080/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej:password@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej:password@example.com:8080/db"_sl, &address, &dbName));
}


TEST_CASE("URL Generation", "[C]][Replicator]") {
    CHECK(alloc_slice(c4address_toURL({"ws"_sl, "foo.com"_sl, 8888, "/bar"_sl})) == "ws://foo.com:8888/bar"_sl);
    CHECK(alloc_slice(c4address_toURL({"ws"_sl, "foo.com"_sl, 0,    "/"_sl}))    == "ws://foo.com/"_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Create C4Replicator without start", "[C][Push]") {
    // For CBL-524 "Lazy c4replicator initialize cause memory leak"
    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;
    _sg.remoteDBName = "something"_sl;

    _repl = c4repl_new(db, _sg.address, _sg.remoteDBName, params, ERROR_INFO(err));
    CHECK(_repl);
    C4Log("---- Releasing C4Replicator ----");
    _repl = nullptr;
}


// Test invalid URL scheme:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid Scheme", "[C][Push][!throws]") {
    ExpectingExceptions x;
    _sg.address.scheme = "http"_sl;
    C4Error err;
    CHECK(!c4repl_isValidRemote(_sg.address, _sg.remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);
}


// Test missing or invalid database name:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid URLs", "[C][Push][!throws]") {
    ExpectingExceptions x;
    _sg.remoteDBName = ""_sl;
    C4Error err;
    CHECK(!c4repl_isValidRemote(_sg.address, _sg.remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);

    _sg.remoteDBName = "Invalid Name"_sl;
    err = {};
    CHECK(!c4repl_isValidRemote(_sg.address, _sg.remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);
}


// Test connection-refused error by connecting to a bogus port of localhost
TEST_CASE_METHOD(ReplicatorAPITest, "API Connection Failure", "[C][Push]") {
    ExpectingExceptions x;
    _sg.address.hostname = C4STR("localhost");
    _sg.address.port = 1;  // wrong port!
    _mayGoOffline = true;

    {
        Encoder enc;
        enc.beginDict();
        enc[kC4ReplicatorOptionMaxRetries] = 3;
        enc[kC4ReplicatorOptionMaxRetryInterval] = 2;
        enc.endDict();
        _options = AllocedDict(enc.finish());
    }

    replicate(kC4Disabled, kC4OneShot, false);

    CHECK(_callbackStatus.error.domain == POSIXDomain);
    CHECK((_callbackStatus.error.code == ECONNREFUSED || _callbackStatus.error.code == ETIMEDOUT));
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
    CHECK(_wentOffline);
    CHECK(_numCallbacksWithLevel[kC4Busy] == 0);
    CHECK(_numCallbacksWithLevel[kC4Idle] == 0);
    CHECK(_numCallbacksWithLevel[kC4Offline] == 3);
}


// Test host-not-found error by connecting to a nonexistent hostname
TEST_CASE_METHOD(ReplicatorAPITest, "API DNS Lookup Failure", "[C][Push]") {
    ExpectingExceptions x;
    _sg.address.hostname = C4STR("qux.ftaghn.miskatonic.edu");
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrUnknownHost);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
    CHECK(_numCallbacksWithLevel[kC4Busy] == 0);
    CHECK(_numCallbacksWithLevel[kC4Idle] == 0);
}

C4_START_WARNINGS_SUPPRESSION
C4_IGNORE_NONNULL

TEST_CASE_METHOD(ReplicatorAPITest, "Set Progress Level Error Handling", "[C][Pull]")
#ifdef __clang__
__attribute__((no_sanitize("nullability-arg"))) // suppress breakpoint passing nullptr below
#endif
{
    C4Error err;
    C4Address addr {kC4Replicator2Scheme, C4STR("localhost"),  4984};
    repl::C4ReplParamsDefaultCollection params;
    params.pull = kC4OneShot;
    c4::ref<C4Replicator> repl = c4repl_new(db, addr, C4STR("db"), params, ERROR_INFO(err));
    REQUIRE(repl);

    CHECK(!c4repl_setProgressLevel(nullptr, kC4ReplProgressPerAttachment, &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorInvalidParameter);

    CHECK(!c4repl_setProgressLevel(repl, (C4ReplicatorProgressLevel)250, &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorInvalidParameter);
}

C4_STOP_WARNINGS_SUPPRESSION

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);

    CHECK(_docsEnded == 100);
    REQUIRE(c4db_getDocumentCount(db2) == 100);
}
#endif


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push & Pull Deletion", "[C][Push][Pull]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    createDB2();
    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);
    CHECK(_docsEnded == 1);

    c4::ref<C4Document> doc = c4db_getDoc(db2, "doc"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}

TEST_CASE_METHOD(ReplicatorAPITest, "Per Collection Context Documents Ended", "[Pull][Sync]") {
    createDB2();
    createRev(db2, "doc"_sl, kRevID, kFleeceBody);

    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    std::vector<std::string> docIDs;
    params.pull = kC4OneShot;
    int overall = 0;
    int perCollection = 0;
    params.callbackContext = &overall;
    params.onDocumentsEnded = [](C4Replicator* repl,
        bool pushing,
        size_t numDocs,
        const C4DocumentEnded* docs[],
        void* context) {
            *((int*)context) = 42;
            if (docs[0]->collectionContext) {
                *((int*)docs[0]->collectionContext) = 24;
            }
    };

    int expectedOverall = 0;
    int expectedPerCollection = 0;

    C4ReplicationCollection coll;
    SECTION("When using a collection, but not setting a per collection context, only overall is used") {
        coll = {
            kC4DefaultCollectionSpec,
            kC4Disabled,
            kC4OneShot
        };

        params.collectionCount = 1;
        params.collections = &coll;
        expectedOverall = 42;
        // perCollection is untouched
    }

    SECTION("When using a collection, and setting a per collection context, both contexts are available") {
        coll = {
            kC4DefaultCollectionSpec,
            kC4Disabled,
            kC4OneShot
        };

        coll.callbackContext = &perCollection;
        params.collectionCount = 1;
        params.collections = &coll;
        expectedOverall = 42;
        expectedPerCollection = 24;
    }

    c4::ref<C4Replicator> repl = c4repl_newLocal(db, db2, params, ERROR_INFO(err));
    REQUIRE(repl);
    REQUIRE(c4repl_setProgressLevel(repl, kC4ReplProgressPerDocument, ERROR_INFO(err)));

    c4repl_start(repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(repl).level == kC4Stopped);
    REQUIRE(c4db_getDocumentCount(db) == 1);
    CHECK(overall == expectedOverall);
    CHECK(perCollection == expectedPerCollection);
}

TEST_CASE_METHOD(ReplicatorAPITest, "API Single Collection Sync", "[Push][Pull][Sync]") {
    createDB2();
    
    C4CollectionSpec Roses = { "roses"_sl, "flowers"_sl };
    auto collRose1 = createCollection(db, Roses);
    auto collRose2 = createCollection(db2, Roses);
    
    addDocs(db, Roses, 10);
    addDocs(db2, Roses, 10);
    
    int expectDocCountInDB = 0;
    int expectDocCountInDB2 = 0;
    
    C4ReplicationCollection coll;
    SECTION("Push") {
        coll = {
            Roses,
            kC4OneShot,
            kC4Disabled,
        };
        expectDocCountInDB = 10;
        expectDocCountInDB2 = 20;
    }
    
    SECTION("Pull") {
        coll = {
            Roses,
            kC4Disabled,
            kC4OneShot
        };
        expectDocCountInDB = 20;
        expectDocCountInDB2 = 10;
    }
    
    C4ReplicatorParameters params{};
    params.collections = &coll;
    params.collectionCount = 1;
    
    C4Error err;
    c4::ref<C4Replicator> repl = c4repl_newLocal(db, db2, params, ERROR_INFO(err));
    REQUIRE(repl);

    c4repl_start(repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(repl).level == kC4Stopped);
    
    CHECK(c4coll_getDocumentCount(collRose1) == expectDocCountInDB);
    CHECK(c4coll_getDocumentCount(collRose2) == expectDocCountInDB2);
}

#endif


TEST_CASE_METHOD(ReplicatorAPITest, "API Custom SocketFactory", "[C][Push][Pull]") {
    _sg.address.hostname = C4STR("localhost");
    struct Context {
        int factoryCalls = 0;
        C4Socket* socket = nullptr;
    };
    Context context;
    C4SocketFactory factory = {};
    factory.context = &context;
    factory.open = [](C4Socket* socket, const C4Address* addr,
                      C4Slice options, void *context) {
        ((Context*)context)->factoryCalls++;
        ((Context*)context)->socket = c4socket_retain(socket);      // Retain the socket
        socket->setNativeHandle((void*)0x12345678);
        c4socket_closed(socket, {NetworkDomain, kC4NetErrTooManyRedirects});
    };
    _socketFactory = &factory;

    replicate(kC4Disabled, kC4OneShot, false);
    REQUIRE(context.factoryCalls == 1);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTooManyRedirects);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);

    // Check that the retained socket still exists, and release it:
    CHECK(context.socket != nullptr);
    CHECK(context.socket->getNativeHandle() == (void*)0x12345678);
    c4socket_release(context.socket);
}


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Filtered Push", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    _pushFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        ((ReplicatorAPITest*)context)->_counter++;
        assert(docID.size > 0);
        assert(revID.size > 0);
        Dict body(flbody);
        assert(body.count() >= 4);
        return body["gender"_sl].asString() == "male"_sl;
    };

    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);

    CHECK(_counter == 100);
    CHECK(_docsEnded == 45);
    CHECK(c4db_getDocumentCount(db2) == 45);
}
#endif

// CBL-221
#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Stop with doc ended callback", "[C][Pull]") {
    createDB2();
    // Need a large enough data set so that the pulled documents come
    // through in more than one batch
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", 15.0, false, db2);
    
    _enableDocProgressNotifications = true;
    
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        ((ReplicatorAPITest*)context)->_docsEnded += (int)numDocs;
        c4repl_stop(repl);
    };
    
    replicate(kC4Disabled, kC4Continuous);
    
    // Not being equal implies that some of the doc ended callbacks failed
    // (presumably because of CBL-221 which causes an actor internal assertion
    // failure that cannot be detected from the outside)
    CHECK(c4db_getDocumentCount(db) == _docsEnded);
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Pending Document IDs", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    FLSliceResult options {};

    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    int expectedPending = 0;
    SECTION("Normal") {
        expectedPending = 100;
    }

    SECTION("Filtered") {
        expectedPending = 99;
        params.pushFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                               C4RevisionFlags flags, FLDict flbody, void *context) {
            return FLSlice_Compare(docID, "0000005"_sl) != 0;
        };
    }

    SECTION("Set Doc IDs") {
        expectedPending = 2;
        FLEncoder e = FLEncoder_New();
        FLEncoder_BeginDict(e, 1);
        FLEncoder_WriteKey(e, FLSTR(kC4ReplicatorOptionDocIDs));
        FLEncoder_BeginArray(e, 2);
        FLEncoder_WriteString(e, FLSTR("0000002"));
        FLEncoder_WriteString(e, FLSTR("0000004"));
        FLEncoder_EndArray(e);
        FLEncoder_EndDict(e);
        options = FLEncoder_Finish(e, nullptr);
        params.replCollection.optionsDictFleece = C4Slice(options);
        FLEncoder_Free(e);
    }

    _repl = c4repl_newLocal(db, (C4Database*)db2, params, ERROR_INFO(err));
    REQUIRE(_repl);

    FLSliceResult_Release(options);

    C4SliceResult encodedDocIDs = c4repl_getPendingDocIDs(_repl, kC4DefaultCollectionSpec, ERROR_INFO(err));
    REQUIRE(encodedDocIDs != nullslice);
    FLArray docIDs = FLValue_AsArray(FLValue_FromData(C4Slice(encodedDocIDs), kFLTrusted));
    CHECK(FLArray_Count(docIDs) == expectedPending);
    c4slice_free(encodedDocIDs);

    c4repl_start(_repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(_repl).level == kC4Stopped);
    encodedDocIDs = c4repl_getPendingDocIDs(_repl, kC4DefaultCollectionSpec, &err);
    CHECK(err.code == 0);
    CHECK(encodedDocIDs == nullslice);
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Is Document Pending", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    FLSliceResult options {};

    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    bool expectedIsPending = true;
    SECTION("Normal") {
        expectedIsPending = true;
    }

    SECTION("Filtered") {
        expectedIsPending = false;
        params.replCollection.callbackContext = this;
        params.pushFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                               C4RevisionFlags flags, FLDict flbody, void *context) {
            auto test = (ReplicatorAPITest*)context;
            c4repl_getStatus(test->_repl);  // If _repl were locked during this callback, this would deadlock
            return FLSlice_Compare(docID, "0000005"_sl) != 0;
        };
    }

    SECTION("Set Doc IDs") {
        expectedIsPending = false;
        FLEncoder e = FLEncoder_New();
        FLEncoder_BeginDict(e, 1);
        FLEncoder_WriteKey(e, FLSTR(kC4ReplicatorOptionDocIDs));
        FLEncoder_BeginArray(e, 2);
        FLEncoder_WriteString(e, FLSTR("0000002"));
        FLEncoder_WriteString(e, FLSTR("0000004"));
        FLEncoder_EndArray(e);
        FLEncoder_EndDict(e);
        options = FLEncoder_Finish(e, nullptr);
        params.replCollection.optionsDictFleece = C4Slice(options);
        FLEncoder_Free(e);
    }

    _repl = c4repl_newLocal(db, (C4Database*)db2, params, ERROR_INFO(err));
    REQUIRE(_repl);

    bool isPending = c4repl_isDocumentPending(_repl, "0000005"_sl, kC4DefaultCollectionSpec,  ERROR_INFO(err));
    CHECK(err.code == 0);
    CHECK(isPending == expectedIsPending);

    c4repl_start(_repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(_repl).level == kC4Stopped);

    isPending = c4repl_isDocumentPending(_repl, "0000005"_sl, kC4DefaultCollectionSpec,  ERROR_INFO(err));
    CHECK(!isPending);
    CHECK(err.code == 0);
}
#endif

#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorAPITest, "Pending Document IDs Non-Existent Collection", "[C][Push]") {
    ExpectingExceptions x;
    // Create collection in the db and import documents
    C4CollectionSpec Kyber = {"kyber"_sl, "crystal"_sl};
    auto collKyber = createCollection(db, Kyber);
    importJSONLines(sFixturesDir + "names_100.json", collKyber);

    // Create the collection in the database to be replicated to
    createDB2();
    createCollection(db2, Kyber);

    // Create collection spec but do not create collection in db
    C4CollectionSpec Republic = {"republic"_sl, "galactic"_sl};

    // Set options for the replication
    C4Error err;
    repl::C4ReplParamsOneCollection params{Kyber};
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    _repl = c4repl_newLocal(db, (C4Database*)db2, params, ERROR_INFO(err));
    REQUIRE(_repl);

    C4SliceResult encodedDocIDs = c4repl_getPendingDocIDs(_repl, Republic, &err);
    CHECK(err.code == kC4ErrorNotOpen);
    c4slice_free(encodedDocIDs);

    c4repl_start(_repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(_repl).level == kC4Stopped);
    encodedDocIDs = c4repl_getPendingDocIDs(_repl, Republic, &err);
    CHECK(err.code == kC4ErrorNotOpen);
}

TEST_CASE_METHOD(ReplicatorAPITest, "Pending Document IDs Multiple Collections", "[C][Push]") {
    // Create collections and import documents to them
    C4CollectionSpec Council = { "council"_sl, "jedi"_sl };
    C4CollectionSpec Federation = { "federation"_sl, "trade"_sl };
    auto collCouncil = createCollection(db, Council);
    auto collFederation = createCollection(db, Federation);
    importJSONLines(sFixturesDir + "names_100.json", collCouncil);
    importJSONLines(sFixturesDir + "wikipedia_100.json", collFederation);

    // Create the collections in the database to be replicated to
    createDB2();
    createCollection(db2, Council);
    createCollection(db2, Federation);

    // Set options for the replication
    // Replicating only the Council collection
    C4Error err;
    repl::C4ReplParamsOneCollection paramsCouncil {Council };
    paramsCouncil.push = kC4OneShot;
    paramsCouncil.pull = kC4Disabled;
    paramsCouncil.callbackContext = this;
    paramsCouncil.socketFactory = _socketFactory;

    // Create replicator config for Federation collection
    // This won't actually be replicated, but is needed to call c4repl_getPendingDocIDs for the collection
    repl::C4ReplParamsOneCollection paramsFederation {Federation };
    paramsFederation.push = kC4OneShot;
    paramsFederation.pull = kC4Disabled;
    paramsCouncil.callbackContext = this;
    paramsCouncil.socketFactory = _socketFactory;

    _repl = c4repl_newLocal(db, (C4Database*)db2, paramsCouncil, ERROR_INFO(err));
    REQUIRE(_repl);

    // Not actually used for replication
    auto replFed = c4::ref{ c4repl_newLocal(db, (C4Database*)db2, paramsFederation, ERROR_INFO(err)) };
    REQUIRE(replFed);

    // Check that collection 1 has the right amount of pending documents
    C4SliceResult encodedDocIDs = c4repl_getPendingDocIDs(_repl, Council, &err);
    CHECK(err.code == 0);
    REQUIRE(encodedDocIDs != nullslice);
    FLArray docIDs = FLValue_AsArray(FLValue_FromData(C4Slice(encodedDocIDs), kFLTrusted));
    CHECK(FLArray_Count(docIDs) == 100);
    c4slice_free(encodedDocIDs);

    // Replicate collection 1
    c4repl_start(_repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(_repl).level == kC4Stopped);

    // Now collection 1 shouldn't have any pending documents
    encodedDocIDs = c4repl_getPendingDocIDs(_repl, Council, &err);
    CHECK(err.code == 0);
    REQUIRE(encodedDocIDs == nullslice);

    // Check that collection 2 still has all the documents pending
    encodedDocIDs = c4repl_getPendingDocIDs(replFed, Federation, ERROR_INFO(err));
    CHECK(err.code == 0);
    REQUIRE(encodedDocIDs != nullslice);
    docIDs = FLValue_AsArray(FLValue_FromData(C4Slice(encodedDocIDs), kFLTrusted));
    CHECK(FLArray_Count(docIDs) == 100);
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Rapid Restarts", "[C][Push][Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    _mayGoOffline = true;
    C4Error err;
    REQUIRE(startReplicator(kC4Continuous, kC4Continuous, WITH_ERROR(&err)));
    waitForStatus(kC4Busy, 5s);
    
    C4ReplicatorActivityLevel expected = kC4Stopped;
    SECTION("Stop / Start") {
        c4repl_stop(_repl);
        c4repl_start(_repl, false);
        expected = kC4Idle;
    }

    SECTION("Stop / Start / Stop") {
        c4repl_stop(_repl);
        c4repl_start(_repl, false);
        c4repl_stop(_repl);
    }

    SECTION("Suspend / Unsuspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_setSuspended(_repl, false);
        expected = kC4Idle;
    }
    
    SECTION("Suspend / Unsuspend / Suspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_setSuspended(_repl, false);
        c4repl_setSuspended(_repl, true);
        expected = kC4Offline;
    }
    
    SECTION("Stop / Suspend") {
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, true);
    }
    
    SECTION("Suspend / Stop") {
        c4repl_setSuspended(_repl, true);
        c4repl_stop(_repl);
    }
    
    SECTION("Stop / Unsuspend") {
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, false);
    }
    
    SECTION("Suspend / Stop / Unsuspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, false);
    }
    
    SECTION("Stop / Stop") {
        c4repl_stop(_repl);
        c4repl_stop(_repl);
    }
    
    SECTION("Offline stop") {
        c4repl_setSuspended(_repl, true);
        waitForStatus(kC4Offline);
        c4repl_stop(_repl);
    }
    
    waitForStatus(expected);
    if(expected != kC4Stopped) {
        c4repl_stop(_repl);
        waitForStatus(kC4Stopped);
    }
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Stop while connect timeout", "[C][Push][Pull]") {
    C4SocketFactory factory = {};
    SECTION("Using framing") {
        factory.open = [](C4Socket* socket, const C4Address* addr,
                          C4Slice options, void *context) {
            // Do nothing, just let things time out....
        };
        
        factory.close = [](C4Socket* socket) {
            // This is a requirement for this test to pass, or the socket will
            // never actually finish "closing"
            c4socket_closed(socket, {});
        };
    }
    
    SECTION("Not using framing") {
        factory.framing = kC4NoFraming;
        factory.open = [](C4Socket* socket, const C4Address* addr,
                          C4Slice options, void *context) {
            // Do nothing, just let things time out....
        };
        
        factory.requestClose = [](C4Socket* socket, int code, C4Slice message) {
            // This is a requirement for this test to pass, or the socket will
            // never actually finish "closing"
            c4socket_closed(socket, {});
        };
    }
    
    _socketFactory = &factory;
    
    C4Error err;
    importJSONLines(sFixturesDir + "names_100.json");
    REQUIRE(startReplicator(kC4Disabled, kC4Continuous, WITH_ERROR(&err)));
    CHECK_BEFORE(2s, c4repl_getStatus(_repl).level == kC4Connecting);
    
    c4repl_stop(_repl);
    
    // This should not take more than 5 seconds, and certainly not more than 8!
    waitForStatus(kC4Stopped, 8s);
    _socketFactory = nullptr;
}

TEST_CASE_METHOD(ReplicatorAPITest, "Stop after transient connect failure", "[C][Push][Pull]") {
    _mayGoOffline = true;
    C4SocketFactory factory = {};
    factory.open = [](C4Socket* socket, const C4Address* addr,
                      C4Slice options, void *context) {
        c4socket_closed(socket, {NetworkDomain, kC4NetErrUnknownHost});
    };
    
    factory.close = [](C4Socket* socket) {
        c4socket_closed(socket, {});
    };
    
    _socketFactory = &factory;
    C4Error err;
    importJSONLines(sFixturesDir + "names_100.json");
    REQUIRE(startReplicator(kC4Disabled, kC4Continuous, WITH_ERROR(&err)));

    _numCallbacksWithLevel[kC4Offline] = 0;
    waitForStatus(kC4Offline);
    c4repl_stop(_repl);
    
    waitForStatus(kC4Stopped);
}

TEST_CASE_METHOD(ReplicatorAPITest, "Calling c4socket_ method after STOP", "[C][Push][Pull]") {
    // c.f. the flow with test case "Stop after transient connect failure"
    _mayGoOffline = true;
    C4SocketFactory factory = {};
    C4Socket* c4socket = nullptr;
    factory.context = &c4socket;
    factory.open = [](C4Socket* socket, const C4Address* addr,
                      C4Slice options, void *context) {
        C4Socket** pp = (C4Socket**)context;
        if (*pp == nullptr) {
            *pp = socket;
            // elongate the lifetime of C4Socket.
            c4socket_retain(socket);
        }
        c4socket_closed(socket, {NetworkDomain, kC4NetErrUnknownHost});
    };

    factory.close = [](C4Socket* socket) {
        c4socket_closed(socket, {});
    };

    _socketFactory = &factory;
    C4Error err;
    importJSONLines(sFixturesDir + "names_100.json");
    REQUIRE(startReplicator(kC4Disabled, kC4Continuous, WITH_ERROR(&err)));

    _numCallbacksWithLevel[kC4Offline] = 0;
    waitForStatus(kC4Offline);
    c4repl_stop(_repl);

    waitForStatus(kC4Stopped);

    // Because of the above c4socket_retain, the lifetime of c4socket is
    // elongated, overliving the Replicator, Connection, and BLIPIO which serves
    // as the delegate to the C4Socket/WebSocketImpl. The following call will crash
    // if we don't use WeakHolder.
    c4socket_gotHTTPResponse(c4socket, 0, nullslice);

    c4socket_release(c4socket);
}

TEST_CASE_METHOD(ReplicatorAPITest, "Set Progress Level", "[Pull][C]") {
    createDB2();

    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    std::vector<std::string> docIDs;
    params.pull = kC4OneShot;
    params.onDocumentsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        auto docIDs = (std::vector<std::string>*)context;
        for(size_t i = 0; i < numDocs; i++) {
            docIDs->emplace_back(slice(docs[i]->docID));
        }
    };

    params.callbackContext = &docIDs;
    c4::ref<C4Replicator> repl = c4repl_newLocal(db, db2, params, ERROR_INFO(err));
    REQUIRE(repl);

    {
        TransactionHelper t(db2);
        constexpr size_t docBufSize = 20;
        constexpr size_t jsonBufSize = 100;
        char docID[docBufSize], json[jsonBufSize];
        for (unsigned i = 1; i <= 50; i++) {
            snprintf(docID, docBufSize, "doc-%03u", i);
            snprintf(json, jsonBufSize, R"({"n":%d, "even":%s})", i, (i%2 ? "false" : "true"));
            createFleeceRev(db2, slice(docID), C4STR("1-abcd"), slice(json));
        }
    }

    c4repl_start(repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(repl).level == kC4Stopped);
    
    REQUIRE(c4db_getLastSequence(db) == 50);
    CHECK(docIDs.empty());
    docIDs.clear();

    REQUIRE(c4repl_setProgressLevel(repl, kC4ReplProgressPerDocument, WITH_ERROR(&err)));

    {
        TransactionHelper t(db2);
        constexpr size_t docBufSize = 20;
        constexpr size_t jsonBufSize = 100;
        char docID[docBufSize], json[jsonBufSize];
        for (unsigned i = 51; i <= 100; i++) {
            snprintf(docID, docBufSize, "doc-%03u", i);
            snprintf(json, jsonBufSize, R"({"n":%d, "even":%s})", i, (i%2 ? "false" : "true"));
            C4Test::createFleeceRev(db2, slice(docID), C4STR("1-abcd"), slice(json));
        }
    }

    c4repl_start(repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(repl).level == kC4Stopped);

    REQUIRE(c4db_getLastSequence(db) == 100);
    REQUIRE(docIDs.size() == 50); 
    for(unsigned i = 0; i < 50; i++) {
        auto nextID = litecore::format( "doc-%03u", i + 51);
        CHECK(nextID == docIDs[i]);
    }
}

TEST_CASE_METHOD(ReplicatorAPITest, "Progress Level vs Options", "[Pull][C]") {
    createDB2();

    C4Error err;
    repl::C4ReplParamsDefaultCollection params;
    std::vector<std::string> docIDs;
    params.pull = kC4OneShot;
    params.onDocumentsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        auto docIDs = (std::vector<std::string>*)context;
        for(size_t i = 0; i < numDocs; i++) {
            docIDs->emplace_back(slice(docs[i]->docID));
        }
    };

    params.callbackContext = &docIDs;

    c4::ref<C4Replicator> repl = c4repl_newLocal(db, db2, params, ERROR_INFO(err));
    REQUIRE(repl);
    REQUIRE(c4repl_setProgressLevel(repl, kC4ReplProgressPerDocument, ERROR_INFO(err)));

    {
        Encoder enc;
        enc.beginDict();
        enc[kC4ReplicatorOptionMaxRetries] = 3;
        enc[kC4ReplicatorOptionMaxRetryInterval] = 2;
        enc.endDict();
        _options = AllocedDict(enc.finish());
    }

    c4repl_setOptions(repl, _options.data());
    {
        TransactionHelper t(db2);
        constexpr size_t docBufSize = 20;
        constexpr size_t jsonBufSize = 100;
        char docID[docBufSize], json[jsonBufSize];
        for (unsigned i = 1; i <= 50; i++) {
            snprintf(docID, docBufSize, "doc-%03u", i);
            snprintf(json, jsonBufSize, R"({"n":%d, "even":%s})", i, (i%2 ? "false" : "true"));
            createFleeceRev(db2, slice(docID), C4STR("1-abcd"), slice(json));
        }
    }

    c4repl_start(repl, false);
    REQUIRE_BEFORE(5s, c4repl_getStatus(repl).level == kC4Stopped);
    REQUIRE(c4db_getLastSequence(db) == 50);
    REQUIRE(docIDs.size() == 50); 
    for(unsigned i = 0; i < 50; i++) {
        auto nextID = litecore::format( "doc-%03u", i + 1);
        CHECK(nextID == docIDs[i]);
    }
}


#include "c4ReplicatorImpl.hh"

struct C4TestReplicator : public litecore::C4ReplicatorImpl {
    C4TestReplicator(C4Database* db, C4ReplicatorParameters params)
        : C4ReplicatorImpl(db, params)   { }
    alloc_slice propertiesMemory() const { return _options->properties.data(); }
    void createReplicator() override     { }
    alloc_slice URL() const override     { return nullslice; }
};

#endif

TEST_CASE_METHOD(ReplicatorAPITest, "Connection Timeout stop properly", "[C][Push][Pull][.Slow]") {
    // CBL-2410
    C4SocketFactory factory = {};
    _mayGoOffline = true;

    SECTION("Using framing") {
        factory.open = [](C4Socket* socket, const C4Address* addr,
                          C4Slice options, void *context) {
            // Do nothing, just let things time out....
        };
        
        factory.close = [](C4Socket* socket) {
            // This is a requirement for this test to pass, or the socket will
            // never actually finish "closing".  Furthermore, this call will hang
            // before this fix
            c4socket_closed(socket, {});
        };
    }
    
    SECTION("Not using framing") {
        factory.framing = kC4NoFraming;
        factory.open = [](C4Socket* socket, const C4Address* addr,
                          C4Slice options, void *context) {
            // Do nothing, just let things time out....
        };
        
        factory.requestClose = [](C4Socket* socket, int code, C4Slice message) {
            // This is a requirement for this test to pass, or the socket will
            // never actually finish "closing".  Furthermore, this call will hang
            // before this fix
            c4socket_closed(socket, {});
        };
    }

    _socketFactory = &factory;
    
    C4Error err;
    importJSONLines(sFixturesDir + "names_100.json");
    REQUIRE(startReplicator(kC4Disabled, kC4OneShot, &err));
    
    // Before the fix, offline would never be reached
    waitForStatus(kC4Offline, 16s);
    c4repl_stop(_repl);
    waitForStatus(kC4Stopped, 2s);
    _socketFactory = nullptr;
}

// CBL-3747: createFleeceRev was creating rev in the default collection if revID
// is null.
TEST_CASE_METHOD(ReplicatorAPITest, "createFleeceRev - null revID", "[C]") {
    C4CollectionSpec collSpec { "nullRevID"_sl, "fleeceRev"_sl };
    auto coll = c4db_createCollection(db, collSpec, ERROR_INFO());
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(coll);
    REQUIRE(defaultColl);
    
    constexpr size_t bufSize = 10;
    for(int i = 0; i < 10; ++i) {
        char docID[bufSize];
        snprintf(docID, bufSize, "doc-%i", i);
        createFleeceRev(coll, slice(docID), nullslice, slice(json5("{revID:'null',create:'fleece'}")));
    }
    
    CHECK(c4coll_getDocumentCount(coll) == 10);
    CHECK(c4coll_getDocumentCount(defaultColl) == 0);
}

class ReplicatorAPITestRemoteReplicator : public C4ReplicatorImpl {
public:
    ReplicatorAPITestRemoteReplicator(C4Database* db NONNULL, const C4ReplicatorParameters& params)
        : C4ReplicatorImpl(db, params) {}

    unsigned maxRetryCount() const {
        return getIntProperty(kC4ReplicatorOptionMaxRetries, 0);
    }

    void createReplicator() override {

    }

    alloc_slice URL() const override {
        return nullslice;
    }
};

TEST_CASE_METHOD(ReplicatorAPITest, "Large 64-bit values in max retry should not turn to zero", "[Replicator][CBL-3872]") {
    Encoder e;
    e.beginDict(1);
    e.writeKey(kC4ReplicatorOptionMaxRetries);
    e.writeUInt(ULLONG_MAX);
    e.endDict();
    auto fleece = e.finish();

    repl::C4ReplParamsDefaultCollection parameters;
    parameters.push = kC4OneShot;
    parameters.optionsDictFleece = fleece;
    ReplicatorAPITestRemoteReplicator replicator(db, parameters);
    CHECK(replicator.maxRetryCount() == UINT_MAX); // 32-bit capped
}
