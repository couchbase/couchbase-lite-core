//
//  ReplicatorCollectionSGTest.cc
//
//  Copyright 2022-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include <cstdint>
#include <memory>

#include "ReplicatorCollectionSGTest.hh"
#include "ReplicatorLoopbackTest.hh"
#include "Base64.hh"
#include "ReplicatorTypes.hh"
#include "c4ReplicatorTypes.h"
#include "catch.hpp"
#include <future>

// Tests in this file, tagged by [.SyncServerCollection], are not done automatically in the
// Jenkins/GitHub CI. They can be run locally with the following environment.
// Option 1: Use docker compose in Replicator/tests/data/docker by running 'docker compose up'.
//
// Option 2: Use your own setup:
//
// Couchbase DB server, with docker, for example,
//   docker run -d --name cbserver -p 8091-8096:8091-8096 -p 11210-11211:11210-11211 couchbase:7.1.1
//   bucket configuration:
//     user    : Administrator
//     password: password
//     name    : any
//     scope   : flowers
//          collections: roses, tulips, lavenders
// Once the DB has been set up, you can run sg_setup.sh, or set up SG manually with the configs below.
// sg_setup.sh should be run with the bucket name as the argument (i.e. './sg_setup.sh couch').
// Sync-gateway:
//   config.json:
/*
 {
   "bootstrap": {
     "server": "couchbase://localhost",
     "username": "Administrator",
     "password": "password",
     "use_tls_server": false
   },
   "logging": {
     "console": {
       "log_level": "info",
       "log_keys": ["*"]
     }
   }
 }
 */
//  config db:
/*
 curl -k --location --request PUT "https://localhost:4985/scratch/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw "{\"num_index_replicas\": 0, \"bucket\": \"$1\", \"scopes\": {\"flowers\": {\"collections\":\
 {\"roses\":{\"sync\":\"function(doc,olddoc){channel(doc.channels)}\"},\
 \"tulips\":{\"sync\":\"function(doc,olddoc){if(doc.isRejected==\"true\")throw({\"forbidden\":\"read_only\"});channel(doc.channels)}"},\
 \"lavenders\":{\"sync\":\"function(doc,olddoc){channel(doc.channels)}\"}}}}}"
 */
//  config SG user:
/*
 curl -k --location --request POST "https://localhost:4985/scratch/_user/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"name": "sguser", "password": "password", "collection_access": {"flowers": {"roses": {"admin_channels": ["*"]}, "tulips": {"admin_channels": ["*"]}, "lavenders": {"admin_channels": ["*"]}}}}'
 */
//
// command argument:
//   [.SyncServerCollection]
//
// You can use the environment variable "NOTLS" and the sg_setup.sh option "-notls" to disable TLS communication
// between CBL and SG. This will enable something like packet analysis with Wireshark.

using namespace std;
using namespace litecore::repl;

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push 5000 Changes Collections SG", "[.SyncServerCollection]") {
    string             idPrefix      = timePrefix();
    const string       docID         = idPrefix + "apipfcc-doc1";
    constexpr unsigned revisionCount = 2000;

    initTest({Roses, Tulips, Lavenders});

    std::vector<string> revIDs{_collectionCount};

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};

    {
        auto              revID = revIDs.begin();
        TransactionHelper t(db);
        for ( C4Collection* coll : _collections ) {
            *revID = createNewRev(coll, slice(docID), nullslice, kFleeceBody);
            REQUIRE(!(revID++)->empty());
        }
    }

    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs);

    C4Log("-------- Mutations --------");
    {
        auto              revID = revIDs.begin();
        TransactionHelper t(db);
        for ( auto coll : _collections ) {
            for ( int i = 2; i <= revisionCount; ++i ) {
                *revID = createNewRev(coll, slice(docID), slice(*revID), kFleeceBody);
                REQUIRE(!revID->empty());
                C4Log("Created rev %s", revID->c_str());
            }
            ++revID;
        }
    }

    C4Log("-------- Second Replication --------");
    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Nonexistent Collection SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    initTest({Roses, Tulips, C4CollectionSpec{"dummy"_sl, FlowersScopeName}});

    for ( auto coll : _collections ) { importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix); }

    ReplParams replParams{_collectionSpecs};
    replParams.collections[0].push = kC4OneShot;
    replParams.collections[1].pull = kC4OneShot;
    replParams.collections[2].push = kC4OneShot;
    replParams.collections[2].pull = kC4OneShot;

    slice   expectedErrorMsg;
    C4Error expectedError;

    SECTION("Collection absent at the remote") {
        expectedErrorMsg = "Collection 'flowers.dummy' is not found on the remote server"_sl;
        expectedError    = {WebSocketDomain, 404};
    }

    SECTION("Collection absent at the local") {
        replParams.collections[2].collection = Lavenders;
        expectedErrorMsg                     = "collection flowers.lavenders is not found in the database."_sl;
        expectedError                        = {LiteCoreDomain, error::NotFound};
    }

    replicate(replParams, false);

    FLStringResult emsg = c4error_getMessage(_callbackStatus.error);
    CHECK(_callbackStatus.error.domain == expectedError.domain);
    CHECK(_callbackStatus.error.code == expectedError.code);
    CHECK(expectedErrorMsg.compare(slice(emsg)) == 0);
    FLSliceResult_Release(emsg);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Bad Configurations SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    initTest({Roses, Tulips, Lavenders});

    for ( auto& coll : _collections ) { importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix); }
    ReplParams replParams{_collectionSpecs};

    C4Error expectedError;
    slice   expectedErrorMsg;

    SECTION("Mixed OneShot and Continuous Modes") {
        replParams.collections[0].push = kC4OneShot;
        replParams.collections[1].pull = kC4OneShot;
        replParams.collections[2].push = kC4Continuous;
        expectedError                  = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg =
                "Invalid replicator configuration: kC4OneShot and kC4Continuous modes cannot be mixed in one replicator."_sl;
    }

    SECTION("Both Sync Directions Disabled") {
        replParams.collections[0].push = kC4OneShot;
        replParams.collections[1].pull = kC4OneShot;
        expectedError                  = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: a collection with both push and pull disabled"_sl;
    }

    SECTION("Mixed Active and Passive Modes") {
        replParams.collections[0].push = kC4Passive;
        replParams.collections[1].push = kC4OneShot;
        replParams.collections[2].pull = kC4OneShot;
        expectedError                  = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg =
                "Invalid replicator configuration: the collection list includes both passive and active ReplicatorMode"_sl;
    }

    SECTION("Duplicated CollectionSpecs") {
        replParams.collections[0].push       = kC4Continuous;
        replParams.collections[1].pull       = kC4Continuous;
        replParams.collections[2].push       = kC4Continuous;
        replParams.collections[2].pull       = kC4Continuous;
        replParams.collections[1].collection = Roses;

        expectedError    = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: the collection list contains duplicated collections."_sl;
    }

    SECTION("Empty CollectionSpecs") {
        replParams.collections[0].push       = kC4Continuous;
        replParams.collections[1].pull       = kC4Continuous;
        replParams.collections[2].push       = kC4Continuous;
        replParams.collections[2].pull       = kC4Continuous;
        replParams.collections[1].collection = {nullslice, nullslice};

        expectedError    = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: a collection without name"_sl;
    }

    replicate(replParams, false);

    C4Error* error = nullptr;
    if ( _errorBeforeStart.code ) {
        error = &_errorBeforeStart;
    } else {
        error = &_callbackStatus.error;
    }
    FLStringResult emsg = c4error_getMessage(*error);
    CHECK(error->domain == expectedError.domain);
    CHECK(error->code == expectedError.code);
    CHECK(expectedErrorMsg.compare(slice(emsg)) == 0);
    FLSliceResult_Release(emsg);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Single Collection SG", "[.SyncServerCollection]") {
    string           idPrefix        = timePrefix();
    constexpr size_t collectionCount = 1;
    constexpr size_t docCount        = 20;

    std::vector<C4CollectionSpec> collectionSpecs{collectionCount};

    bool    continuous = false;
    C4Error expectedError;

    SECTION("Named Collection") { collectionSpecs = {Roses}; }

    // The default scope is not in our SG config. It should be rejected by SG
    SECTION("Default Collection") {
        collectionSpecs = {Default};
        expectedError   = {WebSocketDomain, 400};
    }

    SECTION("Another Named Collection") { collectionSpecs = {Lavenders}; }

    SECTION("Named Collection Continuous") {
        collectionSpecs = {Roses};
        continuous      = true;
    }

    initTest(collectionSpecs);

    importJSONLines(sFixturesDir + "names_100.json", _collections[0], 0, false, docCount, idPrefix);

    updateDocIDs();
    ReplParams replParams{collectionSpecs, continuous ? kC4Continuous : kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);

    if ( continuous ) { _stopWhenIdle.store(true); }
    if ( expectedError.code != 0 ) {
        replicate(replParams, false);
    } else {
        replicate(replParams);
        verifyDocs(_docIDs);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Multiple Collections SG", "[.SyncServerCollection]") {
    string           idPrefix        = timePrefix();
    constexpr size_t collectionCount = 3;
    constexpr size_t docCount        = 20;
    bool             continuous      = false;

    std::vector<C4CollectionSpec> collectionSpecs{collectionCount};

    // Three collections:
    // 1. Guitars - in the default scope. We currently cannot use collections from different scopes
    // 2. Roses   - in scope "flowers"
    // 3. Tulips  - in scope "flowers
    (void)Guitars;

    SECTION("1-2-3") { collectionSpecs = {Lavenders, Roses, Tulips}; }

    SECTION("3-2-1") { collectionSpecs = {Tulips, Roses, Lavenders}; }

    SECTION("2-1-3") {
        collectionSpecs = {Roses, Lavenders, Tulips};
        continuous      = true;
    }

    initTest(collectionSpecs);

    for ( auto& coll : _collections ) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, docCount, idPrefix);
    }

    // Push:
    updateDocIDs();
    ReplParams replParams{collectionSpecs, continuous ? kC4Continuous : kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);

    if ( continuous ) { _stopWhenIdle.store(true); }
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Push & Pull SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    initTest({Lavenders, Roses, Tulips});

    std::vector<unordered_map<alloc_slice, uint64_t>> docIDs{_collectionCount};
    std::vector<unordered_map<alloc_slice, uint64_t>> localDocIDs{_collectionCount};

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        addDocs(_collections[i], 20, idPrefix + "remote-");
        docIDs[i] = getDocIDs(_collections[i]);
    }

    // Send the docs to remote
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(docIDs);
    replicate(replParams);
    verifyDocs(docIDs);

    deleteAndRecreateDBAndCollections();

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        addDocs(_collections[i], 10, idPrefix + "local-");
        localDocIDs[i] = getDocIDs(_collections[i]);
        for ( auto iter = localDocIDs[i].begin(); iter != localDocIDs[i].end(); ++iter ) {
            docIDs[i].emplace(iter->first, iter->second);
        }
    }

    replParams.setPushPull(kC4OneShot, kC4OneShot);
    replParams.setDocIDs(docIDs);
    replicate(replParams);
    // 10 docs are pushed and 20 docs are pulled from each collection.
    CHECK(_callbackStatus.progress.documentCount == 30 * _collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Push + Pull + (Push & Pull) SG",
                 "[.SyncServerCollection]") {
    string             idPrefix  = timePrefix();
    const std::string& channelID = idPrefix;
    initTest({Lavenders, Roses, Tulips}, {channelID});

    enum { iPush, iPull, iPushPull };

    constexpr slice body            = R"({"ans*wer":42})";
    alloc_slice     bodyWithChannel = SG::addChannelToJSON(body, "channels", {channelID});
    alloc_slice     localPrefix{idPrefix + "local-"};
    alloc_slice     remotePrefix{idPrefix + "remote-"};
    unsigned        docCount = 20;

    // push documents to the pull and push/pull collections
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        if ( i == iPush ) { continue; }
        for ( int d = 1; d <= docCount; ++d ) {
            constexpr size_t bufSize = 80;
            char             docID[bufSize];
            snprintf(docID, bufSize, "%.*s%d", SPLAT(remotePrefix), d);
            createFleeceRev(_collections[i], slice(docID), nullslice, bodyWithChannel);
        }
    }

    {
        // Send the docs to remote
        ReplParams replParams{_collectionSpecs};
        replParams.setPushPull(kC4OneShot, kC4Disabled);
        replicate(replParams);
    }

    deleteAndRecreateDBAndCollections();

    // add local docs to Push and Push/Pull collections
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        if ( i == iPull ) { continue; }
        for ( int d = 1; d <= docCount; ++d ) {
            constexpr size_t bufSize = 80;
            char             docID[bufSize];
            snprintf(docID, bufSize, "%.*s%d", SPLAT(localPrefix), d);
            createFleeceRev(_collections[i], slice(docID), nullslice, bodyWithChannel);
        }
    }

    {
        ReplParams replParams{_collectionSpecs};
        replParams.collections[iPush].push     = kC4OneShot;
        replParams.collections[iPull].pull     = kC4OneShot;
        replParams.collections[iPushPull].push = kC4OneShot;
        replParams.collections[iPushPull].pull = kC4OneShot;
        replicate(replParams);
    }

    auto check = [&]() {
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            c4::ref<C4DocEnumerator> e      = c4coll_enumerateAllDocs(_collections[i], nullptr, ERROR_INFO());
            unsigned                 total  = 0;
            unsigned                 local  = 0;
            unsigned                 remote = 0;
            while ( c4enum_next(e, ERROR_INFO()) ) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                slice docID_sl{info.docID};
                total++;
                if ( docID_sl.hasPrefix(localPrefix) ) { local++; }
                if ( docID_sl.hasPrefix(remotePrefix) ) { remote++; }
            }
            switch ( i ) {
                case iPush:
                    CHECK(total == docCount);
                    CHECK(local == docCount);
                    break;
                case iPull:
                    CHECK(total == docCount);
                    CHECK(remote == docCount);
                    break;
                case iPushPull:
                    CHECK(total == 2 * docCount);
                    CHECK(local == docCount);
                    CHECK(remote == docCount);
                    break;
                default:
                    break;
            }
        }
    };
    check();

    // Clear the local database and check again.
    deleteAndRecreateDBAndCollections();
    {
        ReplParams replParams{_collectionSpecs};
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);
    }
    check();
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Push SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    initTest({Lavenders, Roses, Tulips});

    for ( auto& coll : _collections ) { addDocs(coll, 10, idPrefix); }

    updateDocIDs();

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    for ( auto& coll : _collections ) { addDocs(coll, 5, idPrefix); }

    updateDocIDs();

    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Revisions SG",
                 "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();

    initTest({Lavenders, Roses, Tulips});

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        addDocs(_collections[i], 2, idPrefix + "db-" + string(_collectionSpecs[i].name));
    }


    Jthread jthread;
    _callbackWhenIdle = [this, idPrefix, &jthread]() {
        jthread.thread    = std::thread(std::thread{[this, idPrefix]() mutable {
            for ( size_t i = 0; i < _collectionCount; ++i ) {
                const string collName = string(_collectionSpecs[i].name);
                const string docID    = stringprintf("%s-%s-docko", idPrefix.c_str(), collName.c_str());
                ReplicatorLoopbackTest::addRevs(_collections[i], 500ms, alloc_slice(docID), 1, 10, true,
                                                   ("db-"s + collName).c_str());
            }
            _stopWhenIdle.store(true);
        }});
        _callbackWhenIdle = nullptr;
    };

    ReplParams replParams{_collectionSpecs, kC4Continuous, kC4Disabled};
    replicate(replParams);
    // total 3 docs, 12 revs, for each collections.
    CHECK(_callbackStatus.progress.documentCount == 36);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull deltas from Collection SG", "[.SyncCollSlow]") {
    constexpr size_t kDocBufSize = 60;
    // CBG-2643 blocking 1000 docs with 1000 props due to replication taking more than ~1sec
    constexpr int kNumDocs = 799, kNumProps = 799;
    const string  idPrefix  = timePrefix();
    const string  docIDPref = idPrefix + "doc";
    const string  channelID = idPrefix + "a";

    initTest({Roses, Tulips, Lavenders}, {channelID}, "pdfcsg");

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            TransactionHelper t(db);
            std::srand(123456);  // start random() sequence at a known place NOLINT(cert-msc51-cpp)
            for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
                constexpr size_t kDocBufSize = 60;
                char             docID[kDocBufSize];
                snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
                Encoder encPopulate(c4db_createFleeceEncoder(db));
                encPopulate.beginDict();

                encPopulate.writeKey(kC4ReplicatorOptionChannels);
                encPopulate.writeString(channelID);

                for ( int p = 0; p < kNumProps; ++p ) {
                    encPopulate.writeKey(stringprintf("field%03d", p));
                    encPopulate.writeInt(std::rand());
                }
                encPopulate.endDict();
                alloc_slice body  = encPopulate.finish();
                string      revID = createNewRev(_collections[i], slice(docID), body);
            }
        }
    };

    populateDB();

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        JSONEncoder encUpdate;
        encUpdate.beginDict();
        encUpdate.writeKey("docs"_sl);
        encUpdate.beginArray();
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            C4Error             error;
            c4::ref<C4Document> doc =
                    c4coll_getDoc(_collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            encUpdate.beginDict();
            encUpdate.writeKey("_id"_sl);
            encUpdate.writeString(docID);
            encUpdate.writeKey("_rev"_sl);
            encUpdate.writeString(doc->revID);
            for ( Dict::iterator j(props); j; ++j ) {
                encUpdate.writeKey(j.keyString());
                if ( j.keyString() == kC4ReplicatorOptionChannels ) {
                    encUpdate.writeString(j.value().asString());
                    continue;
                }
                auto value = j.value().asInt();
                if ( RandomNumber() % 8 == 0 ) value = RandomNumber();
                encUpdate.writeInt(value);
            }
            encUpdate.endDict();
        }
        encUpdate.endArray();
        encUpdate.endDict();

        REQUIRE(_sg.insertBulkDocs(_collectionSpecs[i], encUpdate.finish(), 30.0));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for ( int pass = 1; pass <= 3; ++pass ) {
        if ( pass == 3 ) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(C4STR(kC4ReplicatorOptionDisableDeltas), true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);

        deleteAndRecreateDBAndCollections();

        populateDB();

        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;

        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);

        double time = st.elapsed();

        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs / time);
        if ( pass == 2 ) timeWithDelta = time;
        else if ( pass == 3 )
            timeWithoutDelta = time;

        for ( size_t i = 0; i < _collectionCount; ++i ) {
            int                      n = 0;
            C4Error                  error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[i], nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while ( c4enum_next(e, ERROR_INFO(error)) ) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                alloc_slice docID = info.docID;
                CHECK(docID.hasPrefix(slice(docIDPref)));
                alloc_slice revID = getLegacyRevID(_collectionSpecs[i], info);
                CHECK(c4rev_getGeneration(revID) == 2);
                ++n;
            }
            CHECK(error.code == 0);
            CHECK(n == kNumDocs);
        }
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed", timeWithDelta, timeWithoutDelta,
          timeWithoutDelta / timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push and Pull Attachments SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();

    initTest({Roses, Tulips, Lavenders});

    // Make sure blob callbacks are used
    _enableBlobProgressNotifications = true;

    std::vector<vector<C4BlobKey>> blobKeys{_collectionCount};  // blobKeys1a, blobKeys1b;

    vector<string> attachments1 = {idPrefix + "Attachment A", idPrefix + "Attachment B", idPrefix + "Attachment Z"};
    {
        const string      doc1 = idPrefix + "doc1";
        const string      doc2 = idPrefix + "doc2";
        TransactionHelper t(db);
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            blobKeys[i] = addDocWithAttachments(db, _collectionSpecs[i], slice(doc1), attachments1, "text/plain");
        }
    }

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replParams.setBlobProgressCallback([](C4Replicator*, bool, C4CollectionSpec collectionSpec, C4String, C4String,
                                          C4BlobKey, uint64_t, uint64_t, C4Error, void* C4NULLABLE) {
        C4Assert((collectionSpec == Roses || collectionSpec == Tulips || collectionSpec == Lavenders));
    });
    replicate(replParams);

    C4Log("-------- Checking docs and attachments --------");
    verifyDocs(_docIDs, true);
    for ( size_t i = 0; i < _collectionCount; ++i ) { checkAttachments(verifyDb, blobKeys[i], attachments1); }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push & Pull Deletion SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docID    = idPrefix + "ppd-doc1";

    initTest({Roses, Tulips, Lavenders});

    for ( auto& coll : _collections ) {
        createRev(coll, slice(docID), kRevID, kFleeceBody);
        createRev(coll, slice(docID), kRev2ID, kEmptyFleeceBody, kRevDeleted);
    }

    std::vector<std::unordered_map<alloc_slice, uint64_t>> docIDs{_collectionCount};

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        docIDs[i] = unordered_map<alloc_slice, uint64_t>{{alloc_slice(docID), 0}};
    }

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    C4Log("-------- Purging & Recreating doc --------");
    for ( auto& coll : _collections ) { REQUIRE(c4coll_purgeDoc(coll, slice(docID), ERROR_INFO())); }

    for ( size_t i = 0; i < _collectionCount; ++i ) { createRev(_collections[i], slice(docID), kRevID, kFleeceBody); }

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    for ( auto& coll : _collections ) {
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, nullptr);
        REQUIRE(remoteDoc);
        CHECK(remoteDoc->revID == kRev2ID);
        CHECK((remoteDoc->flags & kDocDeleted) != 0);
        CHECK((remoteDoc->selectedRev.flags & kRevDeleted) != 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Revoked Revision - SGColl",
                 "[.SyncServerCollection]") {
    const string idPrefix  = timePrefix();
    const string docIDstr  = idPrefix + "apefrr-doc1";
    const string channelID = idPrefix + "a";

    initTest({Roses, Tulips, Lavenders}, {channelID});

    // Setup pull filter to filter the removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    for ( auto& spec : _collectionSpecs ) { REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", {channelID})); }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    // Verify:
    for ( auto& coll : _collections ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
    }
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify if doc1 is not purged as the revoked rev is filtered:
    for ( auto& coll : _collections ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
    }

    // 1 doc per collection
    CHECK(_docsEnded == _collectionCount);
    CHECK(_counter == _collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - w/ and w/o Filter Revoked Revision - SGColl",
                 "[.SyncServerCollection]") {
    const string idPrefix  = timePrefix();
    const string docIDstr  = idPrefix + "apefrr-doc1";
    const string channelID = idPrefix + "a";

    initTest({Roses, Tulips}, {channelID});

    // Push one doc to the remote.
    for ( auto& spec : _collectionSpecs ) { REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", {channelID})); }

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }

        unsigned collIndex = kNotCollectionIndex;
    } cbContexts[2];

    Assert(2 == _collectionCount);
    for ( unsigned i = 0; i < _collectionCount; ++i ) { cbContexts[i].collIndex = i; }

    // Setup pull filter to filter the removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        auto* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            if ( ctx->collIndex == 0 ) {
                return false;
            } else if ( ctx->collIndex == 1 ) {
                return true;
            }
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto  doc = docs[i];
            auto* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    for ( unsigned i = 0; i < _collectionCount; ++i ) {
        replParams.collections[i].pullFilter      = _pullFilter;
        replParams.collections[i].callbackContext = cbContexts + i;
    }
    replicate(replParams);

    // Verify:
    for ( unsigned i = 0; i < _collectionCount; ++i ) {
        // No docs are purged
        CHECK(cbContexts[i].pullFilterTotal == 1);
        CHECK(cbContexts[i].docsEndedTotal == 1);
        CHECK(cbContexts[i].pullFilterPurge == 0);
        CHECK(cbContexts[i].docsEndedPurge == 0);

        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
        cbContexts[i].reset();
    }

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify if doc1 if not not purged in collection 0, but purged in collection 1.
    for ( unsigned i = 0; i < _collectionCount; ++i ) {
        CHECK(cbContexts[i].pullFilterPurge == 1);
        CHECK(cbContexts[i].docsEndedPurge == 1);
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(docIDstr), true, kDocGetAll, nullptr);
        // Purged flags are set with each collection, but for collection 0, it is filtered out,
        // hence, the auto-purge logic is not applied.
        if ( i == 0 ) {
            REQUIRE(doc1);
        } else if ( i == 1 ) {
            REQUIRE(!doc1);
        }
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Revoke Access - SGColl", "[.SyncServerCollection]") {
    const string idPrefix   = timePrefix();
    const string docIDstr   = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "a";
    const string channelIDb = idPrefix + "b";

    initTest({Roses, Tulips, Lavenders}, {channelIDa, channelIDb});

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Put doc in remote DB, in channels a and b
    for ( auto& spec : _collectionSpecs ) { REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", {channelIDa, channelIDb})); }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to channel 'a':
    REQUIRE(_testUser.setChannels({channelIDb}));

    for ( int i = 0; i < _collectionCount; ++i ) {
        // Verify
        alloc_slice revID = getLegacyRevID(_collections[i], slice(docIDstr));
        CHECK(c4rev_getGeneration(revID) == 1);
        // Update doc to only channel 'b'
        REQUIRE(_sg.upsertDoc(_collectionSpecs[i], docIDstr, revID.asString(), "{}", {channelIDb}));
    }

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    for ( auto& coll : _collections ) {
        alloc_slice revID = getLegacyRevID(coll, slice(docIDstr));
        CHECK(revID.hasPrefix("2-"_sl));
    }
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify that doc1 is purged:
    for ( auto& coll : _collections ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(!doc1);
    }
    // One doc per collection
    CHECK(_docsEnded == _collectionCount);
    CHECK(_counter == _collectionCount);
}

#ifdef COUCHBASE_ENTERPRISE

static void validateCipherInputs(ReplicatorCollectionSGTest::CipherContextMap* ctx, C4CollectionSpec& spec,
                                 C4String& docID, C4String& keyPath) {
    auto i = ctx->find(spec);
    REQUIRE(i != ctx->end());

    auto& context = i->second;
    CHECK(spec == context.collection->getSpec());
    CHECK(docID == context.docID);
    CHECK(keyPath == context.keyPath);
    context.called++;
}

C4SliceResult ReplicatorCollectionSGTest::propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID,
                                                        FLDict properties, C4String keyPath, C4Slice input,
                                                        C4StringResult* outAlgorithm, C4StringResult* outKeyID,
                                                        C4Error* outError) {
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->encContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

C4SliceResult ReplicatorCollectionSGTest::propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID,
                                                        FLDict properties, C4String keyPath, C4Slice input,
                                                        C4String algorithm, C4String keyID, C4Error* outError) {
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->decContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Replicate Encrypted Properties with Collections SG",
                 "[.SyncServerCollection]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));
    const string idPrefix = timePrefix();

    initTest({Roses, Tulips, Lavenders});

    encContextMap = std::make_unique<CipherContextMap>();
    decContextMap = std::make_unique<CipherContextMap>();

    std::vector<string> docs{_collectionCount};
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        docs[i] = idPrefix + Options::collectionSpecToPath(_collectionSpecs[i]).asString();
    }
    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;

    {
        TransactionHelper t(db);
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            createFleeceRev(_collections[i], slice(docs[i]), kRevID, originalJSON);
            encContextMap->emplace(std::piecewise_construct, std::forward_as_tuple(_collectionSpecs[i]),
                                   std::forward_as_tuple(_collections[i], docs[i].c_str(), "xNum"));
            decContextMap->emplace(std::piecewise_construct, std::forward_as_tuple(_collectionSpecs[i]),
                                   std::forward_as_tuple(_collections[i], docs[i].c_str(), "xNum"));
        }
    }

    updateDocIDs();

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);
    replicate(replParams);
    verifyDocs(_docIDs, true, TestDecryption ? 2 : 1);

    // Check encryption on active replicator:
    for ( auto& i : *encContextMap ) {
        CipherContext& context = i.second;
        CHECK(context.called > 0);
    }

    // Check decryption on verifyDb:
    for ( auto& i : *decContextMap ) {
        auto&               context = i.second;
        c4::ref<C4Document> doc     = c4coll_getDoc(context.collection, context.docID, true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);

        if ( TestDecryption ) {
            CHECK(context.called > 0);
            CHECK(props.toJSON(false, true) == originalJSON);
        } else {
            CHECK(!context.called);
            CHECK(props.toJSON(false, true)
                  == R"({"encrypted$xNum":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

            // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
            // so the property won't be converted back from the server schema.
            slice       cipherb64 = props["encrypted$xNum"].asDict()["ciphertext"].asString();
            auto        cipher    = base64::decode(cipherb64);
            alloc_slice clear     = ReplicatorLoopbackTest::UnbreakableEncryption(cipher, -1);
            CHECK(clear == "\"123-45-6789\"");
        }
    }
}

static C4SliceResult propEncryptorError(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                        C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                        C4StringResult* outKeyID, C4Error* outError) {
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    auto i    = test->encContextMap->find(spec);
    Assert(i != test->encContextMap->end());

    auto& context = i->second;
    if ( context.called++ == 0 ) {
        Assert(context.simulateError.has_value());
        *outError = *context.simulateError;
        return C4SliceResult(nullslice);
    } else {
        // second time, do normal encryption
        return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
    }
}

static C4SliceResult propDecryptorError(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                        C4String keyPath, C4Slice input, C4String algorithm, C4String keyID,
                                        C4Error* outError) {
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    auto i    = test->decContextMap->find(spec);
    Assert(i != test->decContextMap->end());

    auto& context = i->second;
    if ( context.called++ == 0 ) {
        Assert(context.simulateError.has_value());
        *outError = *context.simulateError;
        return C4SliceResult(nullslice);
    } else {
        // second time, do normal encryption
        return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Encryption Error SG", "[.SyncServerCollection]") {
    const string idPrefix  = timePrefix();
    const string channelID = idPrefix + "ch";

    initTest({Roses}, {channelID});
    // Assertion: _collectionCount == 1

    encContextMap = std::make_unique<CipherContextMap>();

    std::array<string, 3> docs{idPrefix + "01", idPrefix + "02", idPrefix + "03"};
    slice                 clearJSON       = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    alloc_slice           clearBody       = SG::addChannelToJSON(clearJSON, "channels", {channelID});
    alloc_slice           unencryptedBody = SG::addChannelToJSON(R"({"ans*wer": 42})"_sl, "channels", {channelID});

    {
        // Create 3 docs and docs[1] is to be encrypted.
        TransactionHelper t(db);
        for ( unsigned i = 0; i < docs.size(); ++i ) {
            if ( i == 1 ) {
                createFleeceRev(_collections[0], slice(docs[i]), kRevID, clearBody);
                encContextMap->emplace(std::piecewise_construct, std::forward_as_tuple(_collectionSpecs[0]),
                                       std::forward_as_tuple(_collections[0], docs[i].c_str(), "xNum"));
            } else {
                createFleeceRev(_collections[0], slice(docs[i]), kRevID, unencryptedBody);
            }
        }
    }
    updateDocIDs();

    auto fetch = [&]() -> vector<string> {
        resetVerifyDb();
        C4Collection* collection;
        verifyDb->createCollection(_collectionSpecs[0]);
        collection = verifyDb->getCollection(_collectionSpecs[0]);
        Assert(0 == c4coll_getDocumentCount(collection));

        // Pull

        std::vector<C4ReplicationCollection> replCollections{1};
        replCollections[0] = C4ReplicationCollection{_collectionSpecs[0], kC4Disabled, kC4OneShot};
        ReplParams replParams{replCollections};
        replParams.setDocIDs(_docIDs);
        replParams.setOption(kC4ReplicatorOptionDisablePropertyDecryption, true);

        {
            C4Database* savedb = db;
            DEFER { db = savedb; };
            db = verifyDb;
            replicate(replParams);
        }

        vector<string>           ret;
        c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(collection, nullptr, ERROR_INFO());
        while ( c4enum_next(e, ERROR_INFO()) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            ret.push_back(string(info.docID));
        }
        return ret;
    };

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setPropertyEncryptor(propEncryptorError).setPropertyDecryptor(nullptr);

    SECTION("LiteCoreDomain/kC4ErrorCrypto") {
        encContextMap->begin()->second.simulateError = C4Error{LiteCoreDomain, kC4ErrorCrypto};

        _expectedDocPushErrors = {docs[1]};
        {
            ExpectingExceptions x;
            replicate(replParams);
        }
        CHECK(encContextMap->begin()->second.called == 1);
        _expectedDocPushErrors                = {};
        encContextMap->begin()->second.called = 0;

        vector<string> fetchedIDs = fetch();
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        // Second doc is not pushed due to encryption error.
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "03"});

        // Try it again with good encryptor, but crypto errors will move the checkpoint
        // past the doc. The second attempt won't help.
        replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(nullptr);
        replicate(replParams);
        CHECK(encContextMap->begin()->second.called == 0);
        fetchedIDs = fetch();
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "03"});
    }

    SECTION("WebSocketDomain/503") {
        encContextMap->begin()->second.simulateError = C4Error{WebSocketDomain, 503};
        _mayGoOffline                                = true;
        _expectedDocPushErrorsAfterOffline           = {docs[1]};
        {
            ExpectingExceptions x;
            replicate(replParams);
        }
        CHECK(_wentOffline);
        CHECK(encContextMap->begin()->second.called == 2);
        _expectedDocPushErrorsAfterOffline = {};
        vector<string> fetchedIDs          = fetch();
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "02", idPrefix + "03"});
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Decryption Error SG", "[.SyncServerCollection]") {
    const string idPrefix  = timePrefix();
    const string channelID = idPrefix + "ch";

    initTest({Roses}, {channelID});
    // Assertion: _collectionCount == 1

    encContextMap = std::make_unique<CipherContextMap>();

    std::array<string, 3> docs{idPrefix + "01", idPrefix + "02", idPrefix + "03"};
    slice                 clearJSON       = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    alloc_slice           clearBody       = SG::addChannelToJSON(clearJSON, "channels", {channelID});
    alloc_slice           unencryptedBody = SG::addChannelToJSON(R"({"ans*wer": 42})"_sl, "channels", {channelID});

    {
        TransactionHelper t(db);
        for ( unsigned i = 0; i < docs.size(); ++i ) {
            if ( i == 1 ) {
                createFleeceRev(_collections[0], slice(docs[i]), kRevID, clearBody);
                encContextMap->emplace(std::piecewise_construct, std::forward_as_tuple(_collectionSpecs[0]),
                                       std::forward_as_tuple(_collections[0], docs[i].c_str(), "xNum"));
            } else {
                createFleeceRev(_collections[0], slice(docs[i]), kRevID, unencryptedBody);
            }
        }
    }

    // Push the 3 documents to the remote

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(nullptr);
    replicate(replParams);

    deleteAndRecreateDBAndCollections();

    // Fetch and verify

    decContextMap                  = std::make_unique<CipherContextMap>();
    replParams.collections[0].push = kC4Disabled;
    replParams.collections[0].pull = kC4OneShot;
    replParams.setPropertyEncryptor(nullptr).setPropertyDecryptor(propDecryptor);
    decContextMap->emplace(std::piecewise_construct, std::forward_as_tuple(_collectionSpecs[0]),
                           std::forward_as_tuple(_collections[0], docs[1].c_str(), "xNum"));
    replicate(replParams);

    vector<string>           fetchedIDs;
    c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO());
    while ( c4enum_next(e, ERROR_INFO()) ) {
        C4DocumentInfo info;
        c4enum_getDocumentInfo(e, &info);
        fetchedIDs.push_back(string(info.docID));
    }
    std::sort(fetchedIDs.begin(), fetchedIDs.end());
    CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "02", idPrefix + "03"});

    deleteAndRecreateDBAndCollections();
    decContextMap->begin()->second.collection = _collections[0];
    decContextMap->begin()->second.called     = 0;
    replParams.setPropertyEncryptor(nullptr).setPropertyDecryptor(propDecryptorError);
    fetchedIDs.clear();

    SECTION("LiteCoreDomain, kC4ErrorCrypto") {
        decContextMap->begin()->second.simulateError = C4Error{LiteCoreDomain, kC4ErrorCrypto};
        _expectedDocPullErrors                       = {docs[1]};
        {
            ExpectingExceptions x;
            replicate(replParams);
        }

        e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO());
        while ( c4enum_next(e, ERROR_INFO()) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            fetchedIDs.push_back(string(info.docID));
        }
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "03"});

        // Try it again with good decryptor, but crypto errors will move the checkpoint
        // past the doc. The second attempt won't help.
        replParams.setPropertyEncryptor(nullptr).setPropertyDecryptor(propDecryptor);
        _expectedDocPullErrors = {};
        fetchedIDs.clear();
        replicate(replParams);

        e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO());
        while ( c4enum_next(e, ERROR_INFO()) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            fetchedIDs.push_back(string(info.docID));
        }
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "03"});
    }

    SECTION("WebSocketDomain/503") {
        decContextMap->begin()->second.simulateError = C4Error{WebSocketDomain, 503};
        _mayGoOffline                                = true;
        _expectedDocPullErrorsAfterOffline           = {docs[1]};
        {
            ExpectingExceptions x;
            replicate(replParams);
        }
        CHECK(_wentOffline);

        e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO());
        while ( c4enum_next(e, ERROR_INFO()) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            fetchedIDs.push_back(string(info.docID));
        }
        std::sort(fetchedIDs.begin(), fetchedIDs.end());
        CHECK(fetchedIDs == vector<string>{idPrefix + "01", idPrefix + "02", idPrefix + "03"});
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pinned Certificate Success - SGColl", "[.SyncServerCollection]") {
    // Leaf cert (Replicator/tests/data/cert/sg_cert.pem (1st cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIICqzCCAZMCFGrxed0RuxP+uYOzr9wIeRp4gBjHMA0GCSqGSIb3DQEBCwUAMBAx
DjAMBgNVBAMMBUludGVyMB4XDTIyMTAyNTEwMjAzMFoXDTMyMTAyMjEwMjAzMFow
FDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAknbSS/newbZxs4afkUEgMO9WzE1LJAZ7oj3ovLzbsDYVJ3Ct1eBA2yYN
t87ROTvJ85mw4lQ3puMhWGGddYUQzBT7rdtpvydk9aNIefLwU6Yn6YvXC1asxSsb
yFr75j21UZ+qHZ1B4DYAR09Qaps43OKGKJl+4QBUkcLp+Hgo+5e29buv3VvoSK42
MnYsFFtgjVsLBJcL0L9t5gxujPiK8jbdXDYN3Md602rKua9LNwff02w8FWJ8/nLZ
LxtAVidgHJPEY2kDj+S2fUOaAypHcvkHAJ9KKwqHYpwvWzv32WpmmpKBxoiP2NFI
655Efmx7g3pJ2LvUbyOthi8k/VT3/wIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQC3
c+kGcvn3d9QyGYif2CtyAYGRxUQpMjYjqQiwyZmKNp/xErgns5dD+Ri6kEOcq0Zl
MrsPV5iprAKCvEDU6CurGE+sUiJH1csjPx+uCcUlZwT+tZF71IBJtkgfQx2a9Wfs
CA+qS9xaNhuYFkbSIbA5uiSUf9MRxafY8mqjtrOtdPf4fxN5YVsbOzJLtrcVVL9i
Y5rPGtUwixeiZsuGXYkFGLCZx8DWQQrENSu3PI5hshdHgPoHyqxls4yDTDyF3nqq
w9Q3o9L/YDg9NGdW1XQoBgxgKy5G3YT7NGkZXUOJCHsupyoK4GGZQGxtb2eYMg/H
lTIN5f2LxWf+8kJqfjlj
-----END CERTIFICATE-----)");

    // Ensure TLS connection to SGW
    if ( !Address::isSecure(_sg.address) ) { _sg.address = {kC4Replicator2TLSScheme, C4STR("localhost"), 4984}; }
    REQUIRE(Address::isSecure(_sg.address));

    // One-shot push setup
    initTest({Roses});
    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(replParams);

    // Intermediate cert (Replicator/tests/data/cert/sg_cert.pem (2nd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDRzCCAi+gAwIBAgIUQu1TjW0ZRWGCKRQh/JcZxfG/J/YwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAQMQ4wDAYDVQQDDAVJbnRlcjCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAL9WuYxf16AXrxJlSi/hL5cVDWozhfQ2Qb9c5zl3
zPLUmkDkgEq1Yma6pC46jFQsZE1Yqst6iXng/JX4R7azCNFFxyoorDMuynS52VgS
lfAUddIxi86DfM3rkzm/Yho+HoGCeDq+KIKyEQfZmKyVQj8LRQ/qzSAF11B4pp+e
zLD70XRfOZAwJC/utOHxruf+uTr7C3sW8wvW6MDaLsxc/eKptgamMtWe6kM1dkV3
IycEhHHTvrj0dWM7Bwko4OECZkoyzZWHOLNKetlkPQSq2zApHDOQdRin4iAbOGPz
hiJViXiI0pihOJM8yuHF6MuCB8u8JuAvY3c52+OCKQv4hLkCAwEAAaOBjDCBiTAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTLyGcuHP88QhUAmjCgBIwjZj/O2zBX
BgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4wHDEaMBgGA1UEAwwR
Q291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfxW5oFMA0GCSqGSIb3
DQEBCwUAA4IBAQCPDS2j9gQPzXYRNKL9wNiEO5CnrSf2X5b69OoznQRs0R37xUYo
LqFP4/4XFhtNSD6fHhA/pOYC3dIsKNl8+/5Pb4SROsnT6grjbf46bhbVlocKCm0f
gD2TG2OY64eMIpgaSw/WeFQxHmpqm9967iIOg30EqA4zH/hpCHCldFsqhu7FxJ0o
qp/Ps+yRh2PBGVbqkXAabtCnC4yPn1denqCdUPW2/yK7MzDEapMwkwdWVzzaWUy/
LJ46AUTOMWgFdr1+JcCxFKtIXHmL+nSkIlstEkA0jgYOUGSkKB2BxxtrEmnXFTsK
lb78xSgdpAaELOl18IEF5N3FHjVCtvXqStyS
-----END CERTIFICATE-----)");

    replicate(replParams);

    // Root cert (Replicator/tests/data/cert/sg_cert.pem (3rd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDUzCCAjugAwIBAgIUYyeh7cxGMUNIwkVN1PGCx/FbmgUwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAcMRowGAYDVQQDDBFDb3VjaGJhc2UgUm9vdCBDQTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMM9/S7xfgMZF+J4iBxnJEai
cW/FpPsM9HJUt4Xs+JNb+1nJOSo4eGYrAGk/wjxi+VcTdOb/8lrOmT4khKv9CExb
WdxMdSqGb0TM2phd7ZPqCqoMVA0jGJ8ZxLaYlqPsyL9eRio4gVnSE5uNQjWyBEcB
z6eOn1rDZPvJlCF6fRcvgPhFVeIH7xb4jh1OzOoXgM1rrYPLAYr0vLEbk07TwFTE
fCMdBgjEiSnbzQrlgNoVTpcQrGjTmKrN52GC39eTW4tyLdxo+ipgqjiKeTO/qJBp
YZ8V7RgMjhyynIBxhxzZdDEXw5hWZV11kxA3dmBqup9aZ/cK3q2Cxe2mdgMv7aMC
AwEAAaOBjDCBiTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBQQSW+6ctHLjFGg
ZaWLvK61p616HDBXBgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4w
HDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfx
W5oFMA0GCSqGSIb3DQEBCwUAA4IBAQCD+qLQqDkjjVuMDRpvehWr46kKHOHVtXxH
FKpiDDYlD7NUqDWj4y1KKFHZuVg/H+IIflE55jv4ttqmKEMuEpUCd5SS3f9mTM0A
TqwzDVs9HfbuKb6lHtnJLTUvM9wBe/WPW8TCB50AkyMpz5sAAqpK4022Vein2C3T
0uox22kUBslWKZnXMtNeT3h2lFXcCZlQPLRfvHdtXA0t5We2kU0SPiFJc4I0OGjv
zzcNjA18pjiTtpuVeNBUAsBJcbHkNQLKnHGPsBNMAedVCe+AM5CVyZdDlZs//fov
0proEf3d58AqTx4i8uUZHdvmE3MVqeL2rrXFNB74Rs6j8QI1wlpW
-----END CERTIFICATE-----)");

    replicate(replParams);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pinned Certificate Failure - SGColl", "[.SyncServerCollection]") {
    if ( !Address::isSecure(_sg.address) ) { _sg.address = {kC4Replicator2TLSScheme, C4STR("localhost"), 4984}; }
    REQUIRE(Address::isSecure(_sg.address));

    initTest({Roses});

    // Using an unmatched pinned cert:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n"
                     "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n"
                     "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n"
                     "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n"
                     "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n"
                     "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n"
                     "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n"
                     "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n"
                     "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n"
                     "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n"
                     "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n"
                     "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n"
                     "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n"
                     "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n"
                     "S5TnM2/TjtQ=\r\n"
                     "-----END CERTIFICATE-----\r\n";

    // One-shot push setup
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};

    // expectSuccess = false so we can check the error code
    replicate(replParams, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif  //#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Disabled - Revoke Access SG", "[.SyncServerCollection]") {
    const string          idPrefix = timePrefix();
    const string          doc1ID   = idPrefix + "doc1";
    const vector<string>  chIDs{idPrefix};
    constexpr const char* uname = "apdra";

    initTest({Tulips, Roses, Lavenders}, chIDs, uname);

    for ( auto collSpec : _collectionSpecs ) { REQUIRE(_sg.upsertDoc(collSpec, doc1ID, "{}"_sl, chIDs)); }

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } cbContext[3];

    Assert(3 == _collectionCount);

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict,
                                                   void* context) {
        auto ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) { ctx->pullFilterPurge++; }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void*) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto  doc = docs[i];
            auto* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Replication parameters setup
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setOption(kC4ReplicatorOptionAutoPurge, false).setPullFilter(pullFilter);
    for ( size_t i = 0; i < _collectionCount; ++i ) { replParams.setCollectionContext((int)i, cbContext + i); }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(cbContext[i].docsEndedTotal == 1);
        CHECK(cbContext[i].docsEndedPurge == 0);
        CHECK(cbContext[i].pullFilterTotal == 1);
        CHECK(cbContext[i].pullFilterPurge == 0);
    }

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    std::for_each(cbContext, cbContext + _collectionCount, [](CBContext& ctx) { ctx.reset(); });

    replicate(replParams);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(cbContext[i].docsEndedPurge == 1);
        // No pull filter called
        CHECK(cbContext[i].pullFilterTotal == 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Remove Doc From Channel SG", "[.SyncServerCollection]") {
    string         idPrefix = timePrefix();
    string         doc1ID{idPrefix + "doc1"};
    vector<string> chIDs{idPrefix + "a", idPrefix + "b"};

    initTest({Roses, Tulips, Lavenders}, chIDs);

    for ( auto& spec : _collectionSpecs ) { _sg.upsertDoc(spec, doc1ID, "{}"_sl, chIDs); }

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } context;

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void*) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            auto ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec, C4String, C4String, C4RevisionFlags flags,
                                                   FLDict flbody, void* context) {
        auto* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");

    bool       autoPurgeEnabled{true};
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(pullFilter).setCallbackContext(&context);

    SECTION("Auto Purge Enabled") { autoPurgeEnabled = true; }

    SECTION("Auto Purge Disabled") {
        replParams.setOption(C4STR(kC4ReplicatorOptionAutoPurge), false);
        autoPurgeEnabled = false;
    }

    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for ( int i = 0; i < _collectionCount; ++i ) {
        // Verify doc
        alloc_slice revID = getLegacyRevID(_collections[i], slice(doc1ID));
        CHECK(c4rev_getGeneration(revID) == 1);

        // Once verified, remove it from channel 'a' in that collection
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, revID.asString(), "{}", {chIDs[1]});
    }

    C4Log("-------- Pull update");
    context.reset();
    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for ( int i = 0; i < _collectionCount; ++i ) {
        // Verify the update:
        alloc_slice revID = getLegacyRevID(_collections[i], slice(doc1ID));
        CHECK(c4rev_getGeneration(revID) == 2);

        // Remove doc from all channels:
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, revID.asString(), "{}", {});
    }

    C4Log("-------- Pull the removed");
    context.reset();
    replicate(replParams);

    for ( auto& coll : _collections ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);

        if ( autoPurgeEnabled ) {
            // Verify if doc1 is purged:
            REQUIRE(!doc1);
        } else {
            REQUIRE(doc1);
        }
    }

    CHECK(context.docsEndedPurge == _collectionCount);
    if ( autoPurgeEnabled ) {
        CHECK(context.pullFilterPurge == _collectionCount);
    } else {
        // No pull filter called
        CHECK(context.pullFilterTotal == 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Removed Revision SG",
                 "[.SyncServerCollection]") {
    string         idPrefix = timePrefix();
    string         doc1ID   = idPrefix + "doc1";
    vector<string> chIDs{idPrefix + "a"};

    initTest({Roses, Tulips, Lavenders}, chIDs);

    // Create docs on SG:
    for ( auto& spec : _collectionSpecs ) { REQUIRE(_sg.upsertDoc(spec, doc1ID, "{}"_sl, chIDs)); }

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } cbContext;

    // Setup pull filter to filter the _removed rev:
    C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec, C4String, C4String, C4RevisionFlags flags,
                                                   FLDict flbody, void* context) {
        auto ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void*) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            auto ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(pullFilter).setCallbackContext(&cbContext);
    replicate(replParams);

    CHECK(cbContext.docsEndedTotal == _collectionCount);
    CHECK(cbContext.docsEndedPurge == 0);
    CHECK(cbContext.pullFilterTotal == _collectionCount);
    CHECK(cbContext.pullFilterPurge == 0);

    for ( int i = 0; i < _collectionCount; ++i ) {
        // Verify
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);

        // Remove doc from all channels
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, oRevID, "{}", {});
    }

    C4Log("-------- Pull the removed");
    cbContext.reset();
    replicate(replParams);

    // Verify if doc1 is not purged as the removed rev is filtered:
    for ( auto& coll : _collections ) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
    }
    CHECK(cbContext.docsEndedPurge == _collectionCount);
    CHECK(cbContext.pullFilterPurge == _collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled(default) - Delete Doc or Delete then Create Doc SG",
                 "[.SyncServerCollection]") {
    string         idPrefix = timePrefix();
    string         docID    = idPrefix + "doc";
    vector<string> chIDs{idPrefix + "a"};

    initTest({Roses, Tulips, Lavenders}, chIDs);

    alloc_slice bodyJSON = SG::addChannelToJSON("{}"_sl, "channels"_sl, chIDs);

    // Create a doc in each collection
    std::vector<c4::ref<C4Document>> docs{_collectionCount};
    {
        TransactionHelper t(db);
        C4Error           error;
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            docs[i] = c4coll_createDoc(_collections[i], slice(docID), json2fleece(bodyJSON.asString().c_str()), 0,
                                       ERROR_INFO(error));
            REQUIRE(error.code == 0);
            REQUIRE(docs[i]);
        }
    }
    for ( auto& coll : _collections ) { REQUIRE(c4coll_getDocumentCount(coll) == 1); }

    // Push parameter
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    // Push to the remote
    replicate(replParams);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error           error;
        for ( auto& doc : docs ) {
            doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
            REQUIRE(error.code == 0);
        }
    }
    // Verify docs are deleted
    for ( size_t i = 0; i < _collectionCount; ++i ) {
        REQUIRE(docs[i]);
        REQUIRE(docs[i]->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
        REQUIRE(c4coll_getDocumentCount(_collections[i]) == 0);
    }
    // Push the deleted docs
    replicate(replParams);

    bool deleteThenCreate = true;
         SECTION("Delete then Create Doc") {
        // Create a new doc with the same id that was deleted:
        {
            TransactionHelper t(db);
            for ( size_t i = 0; i < _collectionCount; ++i ) {
                C4Error error;
                docs[i] = c4coll_createDoc(_collections[i], slice(docID), json2fleece(bodyJSON.asString().c_str()), 0,
                                                ERROR_INFO(error));
                REQUIRE(error.code == 0);
                REQUIRE(docs[i] != nullptr);
            }
        }
        for ( auto coll : _collections ) { REQUIRE(c4coll_getDocumentCount(coll) == 1); }
    }

    SECTION("Delete Doc") { deleteThenCreate = false; }

    // Perform Pull
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        C4Error             error;
        c4::ref<C4Document> doc2 = c4coll_getDoc(_collections[i], slice(docID), true, kDocGetAll, ERROR_INFO(error));
        CHECK(error.code == 0);
        CHECK(doc2);
        if ( deleteThenCreate ) {
            CHECK(doc2->revID == docs[i]->revID);
            CHECK(c4coll_getDocumentCount(_collections[i]) == 1);
        } else {
            CHECK(doc2->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
            CHECK(c4coll_getDocumentCount(_collections[i]) == 0);
        }
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull multiply-updated SG", "[.SyncServerCollection]") {
    // From <https://github.com/couchbase/couchbase-lite-core/issues/652>:
    // 1. Setup CB cluster & Configure SG
    // 2. Create a document using POST API via SG
    // 3. Create a cblite db on local server using cblite serve
    //      ./cblite/build/cblite serve  --create db.cblite2
    // 4. Replicate between SG -> db.cblite2
    //      ./cblite/build/cblite pull  ws://172.23.100.204:4985/db db.cblite2
    // 5. Validate number of records on db.cblite2 ->Should be  equal to number of documents created in Step2
    // 6. Update existing document using update API via SG (more than twice)
    //      PUT sghost:4985/bd/doc_id?=rev_id
    // 7. run replication between SG -> db.cblite2 again
    // This test must use docID filter instead of channel filter, because channel affects digest-based revID

    const string idPrefix = timePrefix();
    const string docID    = idPrefix + "doc";

    initTest({Roses, Tulips, Lavenders});

    for ( auto& spec : _collectionSpecs ) {
        _sg.upsertDoc(spec, docID + "?new_edits=false", R"({"count":1, "_rev":"1-1111"})");
    }

    std::vector<std::unordered_map<alloc_slice, uint64_t>> docIDs{_collectionCount};

    for ( size_t i = 0; i < _collectionCount; ++i ) {
        docIDs[i] = unordered_map<alloc_slice, uint64_t>{{alloc_slice(docID), 0}};
    }

    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    CHECK(_callbackStatus.progress.documentCount == _collectionCount);
    for ( auto& coll : _collections ) {
        alloc_slice revID = getLegacyRevID(coll, slice(docID));
        CHECK(c4rev_getGeneration(revID) == 1);
    }

    const std::array<std::string, 3> bodies{R"({"count":2, "_rev":"1-1111"})",
                                            R"({"count":3, "_rev":"2-c5557c751fcbfe4cd1f7221085d9ff70"})",
                                            R"({"count":4, "_rev":"3-2284e35327a3628df1ca8161edc78999"})"};

    for ( auto& spec : _collectionSpecs ) {
        for ( const auto& body : bodies ) { _sg.upsertDoc(spec, docID, body); }
    }

    replicate(replParams);
    for ( auto& coll : _collections ) {
        alloc_slice revID = getLegacyRevID(coll, slice(docID));
        CHECK(c4rev_getGeneration(revID) == 4);
    }
}

// This test takes > 1 minute per collection, so I have given it "SyncCollSlow" tag
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull iTunes deltas from Collection SG", "[.SyncCollSlow]") {
    string idPrefix = timePrefix() + "pidfsg";

    initTest({Roses, Tulips, Lavenders});

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        for ( auto& coll : _collections ) {  // Import 5000 docs per collection
            importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", coll, 0, false, 900, idPrefix);
        }
    };
    populateDB();

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    for ( int i = 0; i < _collectionCount; ++i ) {
        auto             numDocs    = c4coll_getDocumentCount(_collections[i]);
        constexpr size_t docBufSize = 50;
        JSONEncoder      enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for ( int docNo = 0; docNo < numDocs; ++docNo ) {
            char docID[docBufSize];
            snprintf(docID, docBufSize, "%s%07u", idPrefix.c_str(), docNo + 1);
            C4Error             error;
            c4::ref<C4Document> doc =
                    c4coll_getDoc(_collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for ( Dict::iterator it(props); it; ++it ) {
                enc.writeKey(it.keyString());
                auto value = it.value();
                if ( it.keyString() == "Play Count"_sl ) enc.writeInt(value.asInt() + 1);
                else
                    enc.writeValue(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        _sg.insertBulkDocs(_collectionSpecs[i], enc.finish(), 300);
    }

    uint64_t numDocs = 0;
    for ( auto& coll : _collections ) { numDocs += c4coll_getDocumentCount(coll); }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for ( int pass = 1; pass <= 3; ++pass ) {
        if ( pass == 3 ) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(kC4ReplicatorOptionDisableDeltas, true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDBAndCollections();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        Stopwatch st;
        replicate(replParams);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs / time);
        if ( pass == 2 ) timeWithDelta = time;
        else if ( pass == 3 )
            timeWithoutDelta = time;
        // Verify docs
        int n = 0;
        for ( auto& coll : _collections ) {
            C4Error                  error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(coll, nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while ( c4enum_next(e, ERROR_INFO(error)) ) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                alloc_slice revID = getLegacyRevID(coll->getSpec(), info);
                CHECK(c4rev_getGeneration(revID) == 2);
                ++n;
            }
            CHECK(error.code == 0);
        }
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed", timeWithDelta, timeWithoutDelta,
          timeWithoutDelta / timeWithDelta);
}

// Disabled pending CBG-
#if 0
// cbl-4499
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull invalid deltas with filter from SG",
                 "[.SyncServerCollection][Delta]") {
    string       idPrefix  = timePrefix();
    const string channelID = idPrefix + "ch";
    initTest({Tulips}, {channelID}, "test_user");

    static constexpr int         kNumDocs = 10, kNumProps = 100;
    static constexpr int         kDocBufSize = 80;
    static constexpr const char* cblTicket   = "cbl-4499";

    const string docPrefix = idPrefix + cblTicket + "_";

    vector<string> docIDs(kNumDocs);

    for (int docNo = 0; docNo < kNumDocs; ++docNo) {
        docIDs[docNo] = stringprintf("%sdoc-%03d", docPrefix.c_str(), docNo);
    }

    // -------- Populating local db --------
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456);  // start random() sequence at a known place
        for (const string& docID : docIDs) {
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(stringprintf("field%03d", p));
                enc.writeInt(std::rand());
            }
            enc.writeKey("channels"_sl);
            enc.beginArray();
            enc.writeString(channelID);
            enc.endArray();
            enc.endDict();
            alloc_slice body  = enc.finish();
            string      revID = createNewRev(_collections[0], slice(docID), body);
        }
    };
    populateDB();

    // Push parameter
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    // Push to the remote
    replicate(replParams);

    // -------- Updating docs on SG --------
    for (const string& docID : docIDs) {
        C4Error             error;
        c4::ref<C4Document> doc = c4coll_getDoc(_collections[0], slice(docID), true, kDocGetAll, ERROR_INFO(error));
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);

        JSONEncoder enc{};
        enc.beginDict();
        for ( Dict::iterator i(props); i; ++i ) {
            enc.writeKey(i.keyString());
            auto value = i.value().asInt();
            if ( RandomNumber() % 2 == 0 ) value = RandomNumber();
            enc.writeInt(value);
        }
        enc.endDict();
        alloc_slice body = enc.finish();

        alloc_slice revID = getLegacyRevID(_collectionSpecs[0], doc);

        _sg.upsertDoc(_collectionSpecs[0], docID, revID.asString(), body, {channelID});
    }

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionName, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) { return true; };

    // -------- Pulling changes from SG --------
#    ifdef LITECORE_CPPTEST
    _expectedDocPullErrors = {docPrefix + "doc-001"};
#    endif
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    {
        ExpectingExceptions x;
        replicate(replParams);
    }

    // Verify
    int                      n = 0;
    C4Error                  error;
    c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO(error));
    REQUIRE(e);
    while ( c4enum_next(e, ERROR_INFO(error)) ) {
        CHECK(error.code == 0);
        C4DocumentInfo info;
        c4enum_getDocumentInfo(e, &info);
        alloc_slice docID = info.docID;
        CHECK(docID.hasPrefix(slice(docPrefix + "doc-")));
        alloc_slice revID = getLegacyRevID(_collectionSpecs[0], info);
        CHECK(revID.hasPrefix("2-"_sl));
        ++n;
    }
    CHECK(n == kNumDocs);
}
#endif

// cbl-4499
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push invalid deltas to SG", "[.SyncServerCollection][Delta]") {
    string       idPrefix  = timePrefix();
    const string channelID = idPrefix + "ch";
    initTest({Tulips}, {channelID}, "test_user");

    static constexpr int         kNumDocs = 10, kNumProps = 100;
    static constexpr int         kDocBufSize = 80;
    static constexpr const char* cblTicket   = "cbl-4499";

    const string docPrefix = idPrefix + cblTicket + "_";

    // -------- Populating local db --------
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456);  // start random() sequence at a known place
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%sdoc-%03d", docPrefix.c_str(), docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(stringprintf("field%03d", p));
                enc.writeInt(std::rand());
            }
            enc.writeKey("channels"_sl);
            enc.beginArray();
            enc.writeString(channelID);
            enc.endArray();
            enc.endDict();
            alloc_slice body  = enc.finish();
            string      revID = createNewRev(_collections[0], slice(docID), body);
        }
    };
    populateDB();

    // Push parameter
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    // Push to the remote
    replicate(replParams);

    // -------- Updating local docs --------
    for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
        char docID[kDocBufSize];
        snprintf(docID, kDocBufSize, "%sdoc-%03d", docPrefix.c_str(), docNo);
        auto                docIDsl = slice(docID);
        TransactionHelper   t(db);
        C4Error             error;
        c4::ref<C4Document> doc = c4coll_getDoc(_collections[0], docIDsl, false, kDocGetAll, ERROR_INFO(error));
        REQUIRE(doc);
        Dict        props = c4doc_getProperties(doc);
        Encoder     enc(c4db_createFleeceEncoder(db));
        MutableDict mutableProps = props.mutableCopy(kFLDeepCopyImmutables);
        for ( Dict::iterator i(props); i; ++i ) {
            auto value = i.value().asInt();
            if ( RandomNumber() % 4 == 0 ) value = RandomNumber();
            mutableProps[i.keyString()] = value;
        }
        enc.writeValue(mutableProps);
        alloc_slice newBody = enc.finish();

        C4String        history = doc->selectedRev.revID;
        C4DocPutRequest rq      = {};
        rq.body                 = newBody;
        rq.docID                = docIDsl;
        rq.revFlags             = (doc->selectedRev.flags & kRevHasAttachments);
        rq.history              = &history;
        rq.historyCount         = 1;
        rq.save                 = true;
        doc                     = c4coll_putDoc(_collections[0], &rq, nullptr, ERROR_INFO(error));
        CHECK(doc);
    }

    // -------- Pushing changes from SG --------
    replicate(replParams);
    CHECK(_callbackStatus.error.code == 0);
    CHECK(_callbackStatus.progress.documentCount == kNumDocs);
}

// CBL-5033
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Revoked docs queue behind revs", "[.SyncServerCollection]") {
    string       idPrefix        = timePrefix();
    const string channelID       = idPrefix + "ch";
    const string channelIDrevoke = channelID + "-revk";
    initTest({Tulips}, {channelIDrevoke, channelID}, "test_user");

    static constexpr int kNumDocs = 1000, kNumProps = 10;
    static constexpr int kDocBufSize = 80;

    // -------- Populating local db --------
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456);  // start random() sequence at a known place //NOLINT(cert-msc51-cpp)
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%sdoc-revk-%03d", idPrefix.c_str(), docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(stringprintf("field%03d", p));
                enc.writeInt(std::rand());  // NOLINT(cert-msc50-cpp)
            }
            enc.writeKey("channels"_sl);
            enc.beginArray();
            enc.writeString(channelIDrevoke);
            enc.endArray();
            enc.endDict();
            alloc_slice body  = enc.finish();
            string      revID = createNewRev(_collections[0], slice(docID), body);
        }
    };
    populateDB();

    // Push to remote
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(replParams);

    // Insert some docs to SG
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%sdoc-%03d", idPrefix.c_str(), docNo);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("channels"_sl);
            enc.beginArray();
            enc.writeString(channelID);
            enc.endArray();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(stringprintf("field%03d", p));
                enc.writeInt(std::rand());  // NOLINT(cert-msc50-cpp)
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        _sg.insertBulkDocs(Tulips, enc.finish());
    }

    // Revoke access to first set of docs (by setting channel to other channel)
    _testUser.setChannels({channelID});

    // Pull revoked + docs that were inserted to SG
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    startReplicator(replParams.paramSetter(), nullptr);

    // Wait for repl to start
    waitForStatus(kC4Busy);

    std::string         finalRevokedID = idPrefix + "doc-revk-999";
    std::string         finalDocID     = idPrefix + "doc-999";
    c4::ref<C4Document> finalRevoked;
    c4::ref<C4Document> finalDoc;

    // Before CBL-5033 changes, finalDoc is inserted after finalRevoked, because revoked come from earlier changes
    //  message, and block queue until all revoked are inserted.
    // After CBL-5033, finalDoc is inserted first, because the code flow gives slight preference to regular revs.
    //
    // Wait until doc is inserted
    while ( !finalDoc ) {
        finalDoc     = c4coll_getDoc(_collections[0], slice(finalDocID), true, kDocGetCurrentRev, nullptr);
        finalRevoked = c4coll_getDoc(_collections[0], slice(finalRevokedID), true, kDocGetCurrentRev, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Assert that final doc to be revoked hasn't been revoked yet
    REQUIRE(finalRevoked);

    while ( finalRevoked ) {
        finalRevoked = c4coll_getDoc(_collections[0], slice(finalRevokedID), true, kDocGetCurrentRev, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(!finalRevoked);

    waitForStatus(kC4Stopped);

    _repl = nullptr;
}

static C4Database* copy_and_open(C4Database* db, const string& idPrefix) {
    const auto   dbPath  = db->getPath();
    const string db2Name = idPrefix + "db2";
    REQUIRE(c4db_copyNamed(dbPath, slice(db2Name), &db->getConfiguration(), ERROR_INFO()));
    return c4db_openNamed(slice(db2Name), &db->getConfiguration(), ERROR_INFO());
}

struct ReplicatorTestDelegate : Replicator::Delegate {
    ~ReplicatorTestDelegate() override = default;

    void replicatorGotHTTPResponse(Replicator* NONNULL, int status, const websocket::Headers& headers) override {}

    void replicatorGotTLSCertificate(slice certData) override{};
    void replicatorStatusChanged(Replicator* NONNULL, const Replicator::Status&) override{};

    void replicatorConnectionClosed(Replicator* NONNULL, const CloseStatus&) override {}

    void replicatorDocumentsEnded(Replicator* NONNULL, const Replicator::DocumentsEnded&) override{};
    void replicatorBlobProgress(Replicator* NONNULL, const Replicator::BlobProgress&) override{};
};

// Wait for a replication to go busy then idle.
static void WaitForRepl(Replicator* repl) {
    int attempts = 5;
    // Wait for busy
    while ( repl->status().level != kC4Busy && attempts-- > 0 ) { std::this_thread::sleep_for(200ms); }
    attempts = 5;
    // Wait for idle
    while ( repl->status().level != kC4Idle && attempts-- > 0 ) { std::this_thread::sleep_for(200ms); }
}

// This sets up two P2P replicators for the below test.
static std::pair<Retained<Replicator>, Retained<Replicator>> PeerReplicators(C4Database* db1, C4Database* db2,
                                                                             Replicator::Delegate& delegate) {
    static atomic<int> validationCount{0};

    auto serverOpts = Replicator::Options::passive(Tulips);
    auto clientOpts = Replicator::Options::pushpull(kC4Continuous, Tulips);

    // Pull filter required to trigger CBL-5448 (delta applied immediately)
    auto pullFilter = [](C4CollectionSpec collectionSpec, FLString docID, FLString revID, C4RevisionFlags flags,
                         FLDict body, void* context) -> bool {
        ++(*(atomic<int>*)context);
        return true;
    };

    serverOpts.collectionOpts[0].callbackContext = &validationCount;
    serverOpts.collectionOpts[0].pullFilter      = pullFilter;

    auto     serverOptsRef = make_retained<Replicator::Options>(serverOpts);
    auto     clientOptsRef = make_retained<Replicator::Options>(clientOpts);
    Retained replServer    = new Replicator(db1, new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Server, 50ms),
                                            delegate, serverOptsRef);
    Retained replClient    = new Replicator(db2, new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Client, 50ms),
                                            delegate, clientOptsRef);
    return std::make_pair(replServer, replClient);
}

/// The below test covers the case of CBL-5448. Here is a rough description of the steps involved:
// 1. Device 2 (passive) creates a doc
// 2. Synced to Device 1 (active)
// 3. Device 1 updates the doc
// 4. Synced back to device 2.
// 5. The incoming rev to device 2 is a delta, and because of pull filter must be applied immediately.
// 6. The peer to peer replication is stopped.
// 7. Device 2 syncs to Sync Gateway <<< At this point, CBL does not send _attachments to SG
// 8. Clear device 1 database
// 9. Pull from SG to Device 1 <<< Here pull fails because the document contains blobs which SG does not know about

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "CBL-5448", "[.SyncServer]") {
    const string idPrefix  = timePrefix();
    const string docID     = idPrefix + "att1";
    const string channelID = idPrefix + "ch";
    initTest({Tulips}, {channelID}, "test_user");

    C4Database* db2 = copy_and_open(db, idPrefix);

    ReplicatorTestDelegate delegate;

    /// SERVER = DB = DEVICE 2
    /// CLIENT = DB2 = DEVICE 1

    auto [replServer, replClient] = PeerReplicators(db, db2, delegate);
    Headers headers;
    headers.add("Set-Cookie"_sl, "flavor=chocolate-chip"_sl);
    LoopbackWebSocket::bind(replServer->webSocket(), replClient->webSocket(), headers);

    // Create doc with blob on Device 2
    // Which is synced by continuous P2P to Device 1
    C4BlobKey blobKey{};
    {
        const std::vector<std::string> attachments = {"Hey, this is an attachment!"};
        TransactionHelper              t(db);
        const auto blobKeys = addDocWithAttachments(db, Tulips, slice(docID), attachments, "text/plain");
        blobKey             = blobKeys[0];
    }
    c4::ref<C4Document> doc = c4coll_getDoc(_collections[0], slice(docID), true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);

    // Add channels to the doc so we can sync
    {
        TransactionHelper t(db);
        auto              props = c4doc_getProperties(doc);
        auto              sk    = db->getFleeceSharedKeys();
        Encoder           enc{};
        enc.setSharedKeys(sk);
        enc.beginDict();
        for ( Dict::iterator j(props); j; ++j ) {
            enc.writeKey(j.keyString());
            enc.writeValue(j.value());
        }
        enc.writeKey("channels");
        enc.beginArray();
        enc.writeString(channelID);
        enc.endArray();
        enc.endDict();
        doc = c4doc_update(doc, enc.finish(), 0, ERROR_INFO());
        REQUIRE(doc);
    }

    replServer->start();
    replClient->start();
    // Wait for sync to complete
    WaitForRepl(replServer);
    replServer->stop();
    replClient->stop();

    // Update doc on Device 1
    {
        TransactionHelper t(db2);
        auto              coll = c4db_getCollection(db2, Tulips, ERROR_INFO());
        REQUIRE(coll);
        doc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        auto    sk = db2->getFleeceSharedKeys();
        Encoder encUpdate{};
        encUpdate.setSharedKeys(sk);
        encUpdate.beginDict();
        auto props = c4doc_getProperties(doc);
        for ( Dict::iterator j(props); j; ++j ) {
            encUpdate.writeKey(j.keyString());
            encUpdate.writeValue(j.value());
        }
        encUpdate.writeKey("Lorem ipsum");
        encUpdate.beginArray();
        encUpdate.writeString(
                "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt");
        encUpdate.writeString("Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque "
                              "laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi "
                              "architecto beatae vitae dicta sunt explicabo.");
        encUpdate.writeString("At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium "
                              "voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint "
                              "occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt "
                              "mollitia animi, id est laborum et dolorum fuga.");
        encUpdate.endArray();
        encUpdate.endDict();
        doc = c4doc_update(doc, encUpdate.finish(), 0, ERROR_INFO());
    }
    REQUIRE(doc);

    // Create another pair of replicators because re-using the existing ones causes SEGFAULT :(
    auto [replServer2, replClient2] = PeerReplicators(db, db2, delegate);
    LoopbackWebSocket::bind(replServer2->webSocket(), replClient2->webSocket(), headers);
    // During this replication, Server (device 2) pulls the doc from Client (device 1).
    // It is here that CBL-5448 is triggered, as Server does not apply `kRevHasAttachments` to the incoming rev.
    replServer2->start();
    replClient2->start();
    // Wait for sync to complete
    WaitForRepl(replServer2);
    replServer2->stop();
    replClient2->stop();

    const alloc_slice updatedRevID = doc->revID;

    // Push doc to sync gateway from Device 2
    ReplParams repl_params{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(repl_params);

    // Device 1 purges it's data
    std::swap(db, db2);
    deleteAndRecreateDBAndCollections();

    // Device 1 pulls the doc from Sync Gateway
    repl_params.setPushPull(kC4Disabled, kC4OneShot);
    replicate(repl_params);

    Retained coll = c4db_getCollection(db, Tulips, ERROR_INFO());
    REQUIRE(coll);
    doc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    REQUIRE(doc->revID == updatedRevID);

    std::swap(db, db2);
    c4db_release(db2);
}
