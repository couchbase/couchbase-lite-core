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

#include <memory>

#include "ReplicatorCollectionSGTest.hh"
#include "ReplicatorLoopbackTest.hh"
#include "Base64.hh"

// Tests in this file, tagged by [.SyncServerCollection], are not done automatically in the
// Jenkins/GitHub CI. They can be run locally with the following environment.
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
 --data-raw "{\"num_index_replicas\": 0, \"bucket\": \"$1\", \"scopes\": {\"flowers\": {\"collections\":{\"roses\":{}, \"tulips\":{}, \"lavenders\":{}}}}}"
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


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push 5000 Changes Collections SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    const string docID = idPrefix + "apipfcc-doc1";
    constexpr unsigned revisionCount = 2000;

    initTest({ Roses, Tulips, Lavenders });

    std::vector<string> revIDs { _collectionCount };

    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };

    {
        auto revID = revIDs.begin();
        TransactionHelper t(db);
        for (C4Collection * coll : _collections) {
            *revID = createNewRev(coll, slice(docID), nullslice, kFleeceBody);
            REQUIRE(!(revID++)->empty());
        }
    }

    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs);

    C4Log("-------- Mutations --------");
    {
        auto revID = revIDs.begin();
        TransactionHelper t(db);
        for (auto coll: _collections) {
            for (int i = 2; i <= revisionCount; ++i) {
                *revID = createNewRev(coll, slice(docID), slice(*revID), kFleeceBody);
                REQUIRE(!revID->empty());
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
    initTest({ Roses,
               Tulips,
               C4CollectionSpec{"dummy"_sl, FlowersScopeName}
            });

    for (auto coll : _collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix);
    }

    ReplParams replParams { _collectionSpecs };
    replParams.collections[0].push = kC4OneShot;
    replParams.collections[1].pull = kC4OneShot;
    replParams.collections[2].push = kC4OneShot;
    replParams.collections[2].pull = kC4OneShot;

    slice expectedErrorMsg;
    C4Error expectedError;

    SECTION("Collection absent at the remote") {
        expectedErrorMsg = "Collection 'flowers.dummy' is not found on the remote server"_sl;
        expectedError = { WebSocketDomain, 404 };
    }

    SECTION("Collection absent at the local") {
        replParams.collections[2].collection = Lavenders;
        expectedErrorMsg = "collection flowers.lavenders is not found in the database."_sl;
        expectedError = { LiteCoreDomain, error::NotFound };
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
    initTest({ Roses,
               Tulips,
               Lavenders
            });

    for (auto& coll : _collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix);
    }
    ReplParams replParams { _collectionSpecs };

    C4Error expectedError;
    slice expectedErrorMsg;

    SECTION("Mixed OneShot and Continuous Modes") {
        replParams.collections[0].push = kC4OneShot;
        replParams.collections[1].pull = kC4OneShot;
        replParams.collections[2].push = kC4Continuous;
        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: kC4OneShot and kC4Continuous modes cannot be mixed in one replicator."_sl;
    }

    SECTION("Both Sync Directions Disabled") {
        replParams.collections[0].push = kC4OneShot;
        replParams.collections[1].pull = kC4OneShot;
        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: a collection with both push and pull disabled"_sl;
    }

    SECTION("Mixed Active and Passive Modes") {
        replParams.collections[0].push = kC4Passive;
        replParams.collections[1].push = kC4OneShot;
        replParams.collections[2].pull = kC4OneShot;
        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: the collection list includes both passive and active ReplicatorMode"_sl;
    }

    SECTION("Duplicated CollectionSpecs") {
        replParams.collections[0].push = kC4Continuous;
        replParams.collections[1].pull = kC4Continuous;
        replParams.collections[2].push = kC4Continuous;
        replParams.collections[2].pull = kC4Continuous;
        replParams.collections[1].collection = Roses;

        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: the collection list contains duplicated collections."_sl;
    }

    SECTION("Empty CollectionSpecs") {
        replParams.collections[0].push = kC4Continuous;
        replParams.collections[1].pull = kC4Continuous;
        replParams.collections[2].push = kC4Continuous;
        replParams.collections[2].pull = kC4Continuous;
        replParams.collections[1].collection = {nullslice, nullslice};

        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
        expectedErrorMsg = "Invalid replicator configuration: a collection without name"_sl;
    }

    replicate(replParams, false);

    C4Error* error = nullptr;
    if (_errorBeforeStart.code) {
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
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    constexpr size_t docCount = 20;

    std::vector<C4CollectionSpec> collectionSpecs { collectionCount };

    bool continuous = false;
    C4Error expectedError;

    SECTION("Named Collection") {
        collectionSpecs = {Roses};
    }

    // The default scope is not in our SG config. It should be rejected by SG
    SECTION("Default Collection") {
        collectionSpecs = {Default};
        expectedError = { WebSocketDomain, 404 };
    }

    SECTION("Another Named Collection") {
        collectionSpecs = {Lavenders};
    }

    SECTION("Named Collection Continuous") {
        collectionSpecs = {Roses};
        continuous = true;
    }

    initTest(collectionSpecs);

    importJSONLines(sFixturesDir + "names_100.json", _collections[0], 0, false, docCount, idPrefix);

    updateDocIDs();
    ReplParams replParams { collectionSpecs, continuous ? kC4Continuous : kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    if (expectedError.code != 0) {
        replicate(replParams, false);
        // Not pass due to CBG-2675
//        FLStringResult emsg = c4error_getMessage(_callbackStatus.error);
//        CHECK(_callbackStatus.error.domain == expectedError.domain);
//        CHECK(_callbackStatus.error.code == expectedError.code);
//        FLSliceResult_Release(emsg);
    } else {
        replicate(replParams);
        verifyDocs(_docIDs);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Multiple Collections SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 3;
    constexpr size_t docCount = 20;
    bool continuous = false;

    std::vector<C4CollectionSpec> collectionSpecs { collectionCount };

    // Three collections:
    // 1. Guitars - in the default scope. We currently cannot use collections from different scopes
    // 2. Roses   - in scope "flowers"
    // 3. Tulips  - in scope "flowers
    (void) Guitars;

    SECTION("1-2-3") {
        collectionSpecs = {Lavenders, Roses, Tulips};
    }

    SECTION("3-2-1") {
        collectionSpecs = {Tulips, Roses, Lavenders};
    }

    SECTION("2-1-3") {
        collectionSpecs = {Roses, Lavenders, Tulips};
        continuous = true;
    }

    initTest(collectionSpecs);

    for (auto& coll : _collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, docCount, idPrefix);
    }

    // Push:
    updateDocIDs();
    ReplParams replParams { collectionSpecs, continuous ? kC4Continuous : kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Push & Pull SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    initTest({Lavenders, Roses, Tulips});

    std::vector<unordered_map<alloc_slice, unsigned>> docIDs { _collectionCount };
    std::vector<unordered_map<alloc_slice, unsigned>> localDocIDs { _collectionCount };

    for (size_t i = 0; i < _collectionCount; ++i) {
        addDocs(_collections[i], 20, idPrefix+"remote-");
        docIDs[i] = getDocIDs(_collections[i]);
    }

    // Send the docs to remote
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(docIDs);
    replicate(replParams);
    verifyDocs(docIDs);

    deleteAndRecreateDBAndCollections();

    for (size_t i = 0; i < _collectionCount; ++i) {
        addDocs(_collections[i], 10, idPrefix+"local-");
        localDocIDs[i] = getDocIDs(_collections[i]);
        for (auto iter = localDocIDs[i].begin(); iter != localDocIDs[i].end(); ++iter) {
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
    string idPrefix = timePrefix();
    const string channelID = idPrefix;
    initTest({ Lavenders,
               Roses,
               Tulips },
             { channelID }
            );
    enum {
        iPush,
        iPull,
        iPushPull
    };

    constexpr slice body = "{\"ans*wer\":42}"_sl;
    alloc_slice bodyWithChannel = SG::addChannelToJSON(body, "channels", {channelID});
    alloc_slice localPrefix {idPrefix + "local-"};
    alloc_slice remotePrefix {idPrefix + "remote-"};
    unsigned docCount = 20;

    // push documents to the pull and push/pull collections
    for (size_t i = 0; i < _collectionCount; ++i) {
        if (i == iPush) {
            continue;
        }
        for (int d = 1; d <= docCount; ++d) {
            constexpr size_t bufSize = 80;
            char docID[bufSize];
            snprintf(docID, bufSize, "%.*s%d", SPLAT(remotePrefix), d);
            createFleeceRev(_collections[i], slice(docID), nullslice, bodyWithChannel);
        }
    }

    {
        // Send the docs to remote
        ReplParams replParams { _collectionSpecs };
        replParams.setPushPull(kC4OneShot, kC4Disabled);
        replicate(replParams);
    }

    deleteAndRecreateDB();
    initTest({Lavenders, Roses, Tulips}, {channelID});

    // add local docs to Push and Push/Pull collections
    for (size_t i = 0; i < _collectionCount; ++i) {
        if (i == iPull) {
            continue;
        }
        for (int d = 1; d <= docCount; ++d) {
            constexpr size_t bufSize = 80;
            char docID[bufSize];
            snprintf(docID, bufSize, "%.*s%d", SPLAT(localPrefix), d);
            createFleeceRev(_collections[i], slice(docID), nullslice, bodyWithChannel);
        }
    }

    {
        ReplParams replParams { _collectionSpecs };
        replParams.collections[iPush].push = kC4OneShot;
        replParams.collections[iPull].pull = kC4OneShot;
        replParams.collections[iPushPull].push = kC4OneShot;
        replParams.collections[iPushPull].pull = kC4OneShot;
        replicate(replParams);
    }

    auto check = [&]() {
        for (size_t i = 0; i < _collectionCount; ++i) {
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[i],
                                                                 nullptr, ERROR_INFO());
            unsigned total = 0;
            unsigned local = 0;
            unsigned remote = 0;
            while (c4enum_next(e, ERROR_INFO())) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                slice docID_sl {info.docID};
                total++;
                if (docID_sl.hasPrefix(localPrefix)) {
                    local++;
                }
                if (docID_sl.hasPrefix(remotePrefix)) {
                    remote++;
                }
            }
            switch (i) {
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
    deleteAndRecreateDB();
    initTest({Lavenders, Roses, Tulips}, {channelID});
    {
        ReplParams replParams { _collectionSpecs };
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);
    }
    check();
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Push SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    initTest({ Lavenders, Roses, Tulips });

    for (auto& coll : _collections) {
        addDocs(coll, 10, idPrefix);
    }

    updateDocIDs();

    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    for (auto& coll : _collections) {
        addDocs(coll, 5, idPrefix);
    }

    updateDocIDs();

    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Revisions SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();

    initTest({ Lavenders, Roses, Tulips });

    for (size_t i = 0; i < _collectionCount; ++i) {
        addDocs(_collections[i], 2, idPrefix + "db-" + string(_collectionSpecs[i].name));
    }


    Jthread jthread;
    _callbackWhenIdle = [=, &jthread]() {
        jthread.thread = std::thread(std::thread{[=]() mutable {
            for (size_t i = 0; i < _collectionCount; ++i) {
                const string collName = string(_collectionSpecs[i].name);
                const string docID = idPrefix + "-" + collName + "-docko";
                ReplicatorLoopbackTest::addRevs(_collections[i], 500ms, alloc_slice(docID), 1, 10, true, ("db-"s + collName).c_str());
            }
            _stopWhenIdle.store(true);
        }});
        _callbackWhenIdle = nullptr;
    };

    ReplParams replParams { _collectionSpecs, kC4Continuous, kC4Disabled };
    replicate(replParams);
    // total 3 docs, 12 revs, for each collections.
    CHECK(_callbackStatus.progress.documentCount == 36);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull deltas from Collection SG", "[.SyncServerCollection]") {
    constexpr size_t kDocBufSize = 60;
    // CBG-2643 blocking 1000 docs with 1000 props due to replication taking more than ~1sec
    constexpr int kNumDocs = 799, kNumProps = 799;
    const string idPrefix = timePrefix();
    const string docIDPref = idPrefix + "doc";
    const string channelID = idPrefix + "a";

    initTest({ Roses, Tulips, Lavenders }, { channelID }, "pdfcsg");

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        for (size_t i = 0; i < _collectionCount; ++i){
            TransactionHelper t(db);
            std::srand(123456); // start random() sequence at a known place NOLINT(cert-msc51-cpp)
            for (int docNo = 0; docNo < kNumDocs; ++docNo) {
                constexpr size_t kDocBufSize = 60;
                char docID[kDocBufSize];
                snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
                Encoder encPopulate(c4db_createFleeceEncoder(db));
                encPopulate.beginDict();

                encPopulate.writeKey(kC4ReplicatorOptionChannels);
                encPopulate.writeString(channelID);

                for (int p = 0; p < kNumProps; ++p) {
                    encPopulate.writeKey(format("field%03d", p));
                    encPopulate.writeInt(std::rand());
                }
                encPopulate.endDict();
                alloc_slice body = encPopulate.finish();
                string revID = createNewRev(_collections[i], slice(docID), body);
            }
        }
    };

    populateDB();

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    for (size_t i = 0; i < _collectionCount; ++i) {
        JSONEncoder encUpdate;
        encUpdate.beginDict();
        encUpdate.writeKey("docs"_sl);
        encUpdate.beginArray();
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            C4Error error;
            c4::ref<C4Document> doc = c4coll_getDoc(_collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            encUpdate.beginDict();
            encUpdate.writeKey("_id"_sl);
            encUpdate.writeString(docID);
            encUpdate.writeKey("_rev"_sl);
            encUpdate.writeString(doc->revID);
            for (Dict::iterator j(props); j; ++j) {
                encUpdate.writeKey(j.keyString());
                if(j.keyString() == kC4ReplicatorOptionChannels){
                    encUpdate.writeString(j.value().asString());
                    continue;
                }
                auto value = j.value().asInt();
                if (RandomNumber() % 8 == 0)
                    value = RandomNumber();
                encUpdate.writeInt(value);
            }
            encUpdate.endDict();
        }
        encUpdate.endArray();
        encUpdate.endDict();

        REQUIRE(_sg.insertBulkDocs(_collectionSpecs[i], encUpdate.finish(), 30.0));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
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

        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        for (size_t i = 0; i < _collectionCount; ++i){
            int n = 0;
            C4Error error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[i], nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while (c4enum_next(e, ERROR_INFO(error))) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                CHECK(slice(info.docID).hasPrefix(slice(docIDPref)));
                CHECK(slice(info.revID).hasPrefix("2-"_sl));
                ++n;
            }
            CHECK(error.code == 0);
            CHECK(n == kNumDocs);
        }
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
        timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push and Pull Attachments SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();

    initTest({ Roses, Tulips, Lavenders });

    std::vector<vector<C4BlobKey>> blobKeys { _collectionCount }; // blobKeys1a, blobKeys1b;

    vector<string> attachments1 = {
            idPrefix + "Attachment A",
            idPrefix + "Attachment B",
            idPrefix + "Attachment Z"
        };
    {
        const string doc1 = idPrefix + "doc1";
        const string doc2 = idPrefix + "doc2";
        TransactionHelper t(db);
        for (size_t i = 0; i < _collectionCount; ++i) {
            blobKeys[i] = addDocWithAttachments(db, _collectionSpecs[i], slice(doc1), attachments1, "text/plain");
        }
    }

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    C4Log("-------- Checking docs and attachments --------");
    verifyDocs(_docIDs, true);
    for (size_t i = 0; i < _collectionCount; ++i) {
        checkAttachments(verifyDb, blobKeys[i], attachments1);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push & Pull Deletion SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "ppd-doc1";

    initTest({ Roses, Tulips, Lavenders });

    for(auto& coll : _collections) {
        createRev(coll, slice(docID), kRevID, kFleeceBody);
        createRev(coll, slice(docID), kRev2ID, kEmptyFleeceBody, kRevDeleted);
    }

    std::vector<std::unordered_map<alloc_slice, unsigned>> docIDs { _collectionCount };

    for(size_t i = 0; i < _collectionCount; ++i) {
        docIDs[i] = unordered_map<alloc_slice, unsigned> {{ alloc_slice(docID), 0 }};
    }

    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDBAndCollections();

    for(size_t i = 0; i < _collectionCount; ++i){
        createRev(_collections[i], slice(docID), kRevID, kFleeceBody);
    }

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    for(auto& coll : _collections) {
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, nullptr);
        REQUIRE(remoteDoc);
        CHECK(remoteDoc->revID == kRev2ID);
        CHECK((remoteDoc->flags & kDocDeleted) != 0);
        CHECK((remoteDoc->selectedRev.flags & kRevDeleted) != 0);
        REQUIRE(c4doc_selectParentRevision(remoteDoc));
        CHECK(remoteDoc->selectedRev.revID == kRevID);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Resolve Conflict SG", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();

    initTest({ Roses, Tulips, Lavenders });

    std::vector<string> collNames { _collectionCount };

    for (size_t i = 0; i < _collectionCount; ++i) {
        collNames[i] = idPrefix + Options::collectionSpecToPath(_collectionSpecs[i]).asString();
        createFleeceRev(_collections[i], slice(collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(_collections[i], slice(collNames[i]), revOrVersID("2-12121212", "1@cafe"),
                        "{\"db\":\"remote\"}"_sl);
    }

    updateDocIDs();

    // Send the docs to remote
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    verifyDocs(_docIDs, true);

    deleteAndRecreateDBAndCollections();

    for (size_t i = 0; i < _collectionCount; ++i) {
        createFleeceRev(_collections[i], slice(collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(_collections[i], slice(collNames[i]), revOrVersID("2-13131313", "1@babe"),
                        "{\"db\":\"local\"}"_sl);
    }

    updateDocIDs(); // Might need to delete this
    replParams.setDocIDs(_docIDs);

    _conflictHandler = [&](const C4DocumentEnded* docEndedWithConflict) {
        C4Error error;
        int i = -1;
        for (int k = 0; k < _collectionCount; ++k) {
            if (docEndedWithConflict->collectionSpec == _collectionSpecs[k]) {
                i = k;
            }
        }
        Assert(i >= 0, "Internal logical error");

        TransactionHelper t(db);

        slice docID = docEndedWithConflict->docID;
        // Get the local doc. It is the current revision
        c4::ref<C4Document> localDoc = c4coll_getDoc(_collections[i], docID, true, kDocGetAll, WITH_ERROR(error));
        CHECK(error.code == 0);

        // Get the remote doc. It is the next leaf revision of the current revision.
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(_collections[i], docID, true, kDocGetAll, &error);
        bool succ = c4doc_selectNextLeafRevision(remoteDoc, true, false, &error);
        Assert(remoteDoc->selectedRev.revID == docEndedWithConflict->revID);
        CHECK(error.code == 0);
        CHECK(succ);

        C4Document* resolvedDoc = remoteDoc;

        FLDict mergedBody = c4doc_getProperties(resolvedDoc);
        C4RevisionFlags mergedFlags = resolvedDoc->selectedRev.flags;
        alloc_slice winRevID = resolvedDoc->selectedRev.revID;
        alloc_slice lostRevID = (resolvedDoc == remoteDoc) ? localDoc->selectedRev.revID
                                                           : remoteDoc->selectedRev.revID;
        bool result = c4doc_resolveConflict2(localDoc, winRevID, lostRevID,
                                             mergedBody, mergedFlags, &error);
        Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s' in '%.*s.%.*s'",
               SPLAT(docID), SPLAT(_collectionSpecs[i].scope), SPLAT(_collectionSpecs[i].name));
        Assert((localDoc->flags & kDocConflicted) == 0);

        if (!c4doc_save(localDoc, 0, &error)) {
            Assert(false, "conflictHandler: c4doc_save failed for '%.*s' in '%.*s.%.*s'",
                   SPLAT(docID), SPLAT(_collectionSpecs[i].scope), SPLAT(_collectionSpecs[i].name));
        }
    };

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    for (size_t i = 0; i < _collectionCount; ++i) {
        c4::ref<C4Document> doc = c4coll_getDoc(_collections[i], slice(collNames[i]),
                                                        true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK(fleece2json(c4doc_getRevisionBody(doc)) == "{db:\"remote\"}"); // Remote Wins
        REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Update Once-Conflicted Doc - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "uocd-doc";
    const string channelID = idPrefix + "uocd";

    initTest({ Roses, Tulips, Lavenders }, { channelID });

    std::array<std::string, 4> bodies {
            R"({"_rev":"1-aaaa","foo":1})",
            R"({"_revisions":{"start":2,"ids":["bbbb","aaaa"]},"foo":2.1})",
            R"({"_revisions":{"start":2,"ids":["cccc","aaaa"]},"foo":2.2})",
            R"({"_revisions":{"start":3,"ids":["dddd","cccc"]},"_deleted":true})"
    };

    // Create a conflicted doc on SG, and resolve the conflict
    for(auto& spec : _collectionSpecs) {
        for(const auto& body : bodies) {
            _sg.upsertDoc(spec, docID + "?new_edits=false", body, { channelID });
        }
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replicate(replParams);

    // Verify doc:
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, nullptr);
        REQUIRE(doc);
        C4Slice revID = C4STR("2-bbbb");
        CHECK(doc->revID == revID);
        CHECK((doc->flags & kDocDeleted) == 0);
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == "1-aaaa"_sl);
    }

    // Update doc:
    alloc_slice body = SG::addChannelToJSON(R"({"ans*wer":42})"_sl, "channels"_sl, { channelID });
    const std::string bodyStr = body.asString();
    for(auto& coll : _collections) {
        createRev(coll, slice(docID), "3-ffff"_sl, json2fleece(bodyStr.c_str()));
    }

    // Push change back to SG:
    C4Log("-------- Pushing");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    updateDocIDs();

    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Revoked Revision - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apefrr-doc1";
    const string channelID = idPrefix + "a";

    initTest({ Roses, Tulips, Lavenders }, { channelID });

    // Setup pull filter to filter the removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    for(auto& spec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", { channelID }));
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    // Verify:
    for(auto& coll : _collections) {
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
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
    }

    // 1 doc per collection
    CHECK(_docsEnded == _collectionCount);
    CHECK(_counter == _collectionCount);
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - w/ and w/o Filter Revoked Revision - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apefrr-doc1";
    const string channelID = idPrefix + "a";

    initTest({ Roses,
               Tulips },
             { channelID }
            );

    // Push one doc to the remote.
    for(auto& spec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", { channelID }));
    }

    struct CBContext {
        int docsEndedTotal = 0;
        int docsEndedPurge = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;
        void reset() {
            docsEndedTotal = 0;
            docsEndedPurge = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
        unsigned collIndex;
    } cbContexts[2];
    Assert( 2 == _collectionCount );
    for (unsigned i = 0; i < _collectionCount; ++i) {
        cbContexts[i].collIndex = i;
    }

    // Setup pull filter to filter the removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        CBContext* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            if (ctx->collIndex == 0) {
                return false;
            } else if (ctx->collIndex == 1) {
                return true;
            }
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            CBContext* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    for (unsigned i = 0; i < _collectionCount; ++i) {
        replParams.collections[i].pullFilter = _pullFilter;
        replParams.collections[i].callbackContext = cbContexts+i;
    }
    replicate(replParams);

    // Verify:
    for (unsigned i = 0; i < _collectionCount; ++i) {
        // No docs are purged
        CHECK(cbContexts[i].pullFilterTotal == 1);
        CHECK(cbContexts[i].docsEndedTotal == 1);
        CHECK(cbContexts[i].pullFilterPurge == 0);
        CHECK(cbContexts[i].docsEndedPurge == 0);

        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(docIDstr), true,
                                                 kDocGetAll, nullptr);
        REQUIRE(doc1);
        cbContexts[i].reset();
    }

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify if doc1 if not not purged in collection 0, but purged in collection 1.
    for(unsigned i = 0; i < _collectionCount; ++i) {
        CHECK(cbContexts[i].pullFilterPurge == 1);
        CHECK(cbContexts[i].docsEndedPurge == 1);
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(docIDstr),
                                                 true, kDocGetAll, nullptr);
        // Purged flags are set with each collection, but for collection 0, it is filtered out,
        // hence, the auto-purge logic is not applied.
        if (i == 0) {
            REQUIRE(doc1);
        } else if (i == 1) {
            REQUIRE(!doc1);
        }
    }
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Revoke Access - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "a";
    const string channelIDb = idPrefix + "b";

    initTest({ Roses, Tulips, Lavenders }, { channelIDa, channelIDb });

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    // Put doc in remote DB, in channels a and b
    for(auto& spec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc( spec, docIDstr, "{}", { channelIDa, channelIDb } ));
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to channel 'a':
    REQUIRE(_testUser.setChannels({ channelIDb }));

    for(int i = 0; i < _collectionCount; ++i) {
        // Verify
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
        CHECK(slice(doc1->revID).hasPrefix("1-"_sl));

        // Update doc to only channel 'b'
        auto oRevID = slice(doc1->revID).asString();
        REQUIRE(_sg.upsertDoc(_collectionSpecs[i], docIDstr, oRevID, "{}", { channelIDb }));
    }

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
        CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    }
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify that doc1 is purged:
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(!doc1);
    }
    // One doc per collection
    CHECK(_docsEnded == _collectionCount);
    CHECK(_counter == _collectionCount);
}

#ifdef COUCHBASE_ENTERPRISE

static void validateCipherInputs(ReplicatorCollectionSGTest::CipherContextMap* ctx,
                                 C4CollectionSpec& spec, C4String& docID, C4String& keyPath) {
    auto i = ctx->find(spec);
    REQUIRE(i != ctx->end());

    auto& context = i->second;
    CHECK(spec == context.collection->getSpec());
    CHECK(docID == context.docID);
    CHECK(keyPath == context.keyPath);
    context.called = true;
}

C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                            C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                            C4StringResult* outKeyID, C4Error* outError)
{
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->encContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                            C4String keyPath, C4Slice input, C4String algorithm,
                            C4String keyID, C4Error* outError)
{
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->decContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Replicate Encrypted Properties with Collections SG", "[.SyncServerCollection]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));
    const string idPrefix = timePrefix();

    initTest({ Roses, Tulips, Lavenders });

    encContextMap = std::make_unique<CipherContextMap>();
    decContextMap = std::make_unique<CipherContextMap>();

    std::vector<string> docs { _collectionCount };
    for(size_t i = 0; i < _collectionCount; ++i) {
        docs[i] = idPrefix + Options::collectionSpecToPath(_collectionSpecs[i]).asString();
    }
    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;

    {
        TransactionHelper t(db);
        for (size_t i = 0; i < _collectionCount; ++i) {
            createFleeceRev(_collections[i], slice(docs[i]), kRevID, originalJSON);
            encContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(_collectionSpecs[i]),
                                   std::forward_as_tuple(_collections[i], docs[i].c_str(), "xNum", false));
            decContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(_collectionSpecs[i]),
                                   std::forward_as_tuple(_collections[i], docs[i].c_str(), "xNum", false));
        }
    }

    updateDocIDs();

    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);
    replicate(replParams);

    verifyDocs(_docIDs, true, TestDecryption ? 2 : 1);

    // Check encryption on active replicator:
    for (auto& i : *encContextMap) {
        CipherContext& context = i.second;
        CHECK(context.called);
    }

    // Check decryption on verifyDb:
    for (auto& i : *decContextMap) {
        auto& context = i.second;
        c4::ref<C4Document> doc = c4coll_getDoc(context.collection, context.docID, true,
                                                kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);

        if (TestDecryption) {
            CHECK(context.called);
            CHECK(props.toJSON(false, true) == originalJSON);
        } else {
            CHECK(!context.called);
            CHECK(props.toJSON(false, true) == R"({"encrypted$xNum":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

            // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
            // so the property won't be converted back from the server schema.
            slice cipherb64 = props["encrypted$xNum"].asDict()["ciphertext"].asString();
            auto cipher = base64::decode(cipherb64);
            alloc_slice clear = ReplicatorLoopbackTest::UnbreakableEncryption(cipher, -1);
            CHECK(clear == "\"123-45-6789\"");
        }
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
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984};
    }
    REQUIRE(Address::isSecure(_sg.address));

    // One-shot push setup
    initTest({ Roses });
    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
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
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984 };
    }
    REQUIRE(Address::isSecure(_sg.address));

    initTest({ Roses });

    // Using an unmatched pinned cert:
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n" \
        "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n" \
        "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n" \
        "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n" \
        "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n" \
        "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n" \
        "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n" \
        "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n" \
        "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n" \
        "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n" \
        "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n" \
        "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n" \
        "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n" \
        "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n" \
        "S5TnM2/TjtQ=\r\n"                                                     \
        "-----END CERTIFICATE-----\r\n";

    // One-shot push setup
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };

    // expectSuccess = false so we can check the error code
    replicate(replParams, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif //#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Disabled - Revoke Access SG",
                 "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string doc1ID = idPrefix + "doc1";
    const vector<string> chIDs { idPrefix };
    constexpr const char * uname = "apdra";

    initTest({ Tulips, Roses, Lavenders }, chIDs, uname);

    for (auto collSpec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc(collSpec, doc1ID, "{}"_sl, chIDs));
    }

    struct CBContext {
        int docsEndedTotal = 0;
        int docsEndedPurge = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;
        void reset() {
            docsEndedTotal = 0;
            docsEndedPurge = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } cbContext[3];
    Assert( 3 == _collectionCount);

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict, void *context)
    {
        auto ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void*) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            CBContext* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };

    // Replication parameters setup
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setOption(kC4ReplicatorOptionAutoPurge, false).setPullFilter(pullFilter);
    for (size_t i = 0; i < _collectionCount; ++i) {
        replParams.setCollectionContext((int)i, cbContext+i);
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    for (size_t i = 0; i < _collectionCount; ++i) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(cbContext[i].docsEndedTotal == 1);
        CHECK(cbContext[i].docsEndedPurge == 0);
        CHECK(cbContext[i].pullFilterTotal == 1);
        CHECK(cbContext[i].pullFilterPurge == 0);
    }

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    std::for_each(cbContext, cbContext + _collectionCount, [](CBContext& ctx) {
        ctx.reset();
    });

    replicate(replParams);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    for (size_t i = 0; i < _collectionCount; ++i) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(cbContext[i].docsEndedPurge == 1);
        // No pull filter called
        CHECK(cbContext[i].pullFilterTotal == 0);
    }
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Remove Doc From Channel SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    string        doc1ID {idPrefix + "doc1"};
    vector<string> chIDs {idPrefix+"a", idPrefix+"b"};

    initTest({ Roses, Tulips, Lavenders }, chIDs);

    for (auto& spec : _collectionSpecs) {
        _sg.upsertDoc(spec, doc1ID, "{}"_sl, chIDs);
    }

    struct CBContext {
        int docsEndedTotal = 0;
        int docsEndedPurge = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;
        void reset() {
            docsEndedTotal = 0;
            docsEndedPurge = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } context;


    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void*) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            auto ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict flbody, void *context)
    {
        CBContext* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");

    bool autoPurgeEnabled {true};
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setPullFilter(pullFilter).setCallbackContext(&context);

    SECTION("Auto Purge Enabled") {
        autoPurgeEnabled = true;
    }

    SECTION("Auto Purge Disabled") {
        replParams.setOption(C4STR(kC4ReplicatorOptionAutoPurge), false);
        autoPurgeEnabled = false;
    }

    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for(int i = 0; i < _collectionCount; ++i) {
        // Verify doc
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(c4rev_getGeneration(doc1->revID) == 1);

        // Once verified, remove it from channel 'a' in that collection
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", { chIDs[1] });
    }

    C4Log("-------- Pull update");
    context.reset();
    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for(int i = 0; i < _collectionCount; ++i) {
        // Verify the update:
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(c4rev_getGeneration(doc1->revID) == 2);

        // Remove doc from all channels:
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", {});
    }

    C4Log("-------- Pull the removed");
    context.reset();
    replicate(replParams);

    for(auto& coll : _collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);

        if (autoPurgeEnabled) {
            // Verify if doc1 is purged:
            REQUIRE(!doc1);
        } else {
            REQUIRE(doc1);
        }
    }

    CHECK(context.docsEndedPurge == _collectionCount);
    if(autoPurgeEnabled) {
        CHECK(context.pullFilterPurge == _collectionCount);
    } else {
        // No pull filter called
        CHECK(context.pullFilterTotal == 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Removed Revision SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    string doc1ID = idPrefix + "doc1";
    vector<string> chIDs {idPrefix+"a"};

    initTest({ Roses, Tulips, Lavenders }, chIDs);

    // Create docs on SG:
    for (auto& spec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, doc1ID, "{}"_sl, chIDs));
    }

    struct CBContext {
         int docsEndedTotal = 0;
         int docsEndedPurge = 0;
         int pullFilterTotal = 0;
         int pullFilterPurge = 0;
         void reset() {
             docsEndedTotal = 0;
             docsEndedPurge = 0;
             pullFilterTotal = 0;
             pullFilterPurge = 0;
         }
     } cbContext;

    // Setup pull filter to filter the _removed rev:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict flbody, void *context)
    {
        auto ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void*) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            auto ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setPullFilter(pullFilter).setCallbackContext(&cbContext);
    replicate(replParams);

    CHECK(cbContext.docsEndedTotal == _collectionCount);
    CHECK(cbContext.docsEndedPurge == 0);
    CHECK(cbContext.pullFilterTotal == _collectionCount);
    CHECK(cbContext.pullFilterPurge == 0);

    for(int i = 0; i < _collectionCount; ++i) {
        // Verify
        c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);

        // Remove doc from all channels
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(_collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", {});
    }

    C4Log("-------- Pull the removed");
    cbContext.reset();
    replicate(replParams);

    // Verify if doc1 is not purged as the removed rev is filtered:
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
    }
    CHECK(cbContext.docsEndedPurge == _collectionCount);
    CHECK(cbContext.pullFilterPurge == _collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled(default) - Delete Doc or Delete then Create Doc SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    string docID = idPrefix + "doc";
    vector<string> chIDs {idPrefix+"a"};

    initTest({ Roses, Tulips, Lavenders }, chIDs);

    alloc_slice bodyJSON = SG::addChannelToJSON("{}"_sl, "channels"_sl, chIDs);

    // Create a doc in each collection
    std::vector<c4::ref<C4Document>> docs { _collectionCount };
    {
        TransactionHelper t(db);
        C4Error error;
        for (size_t i = 0; i < _collectionCount; ++i) {
            docs[i] = c4coll_createDoc(_collections[i], slice(docID),
                                       json2fleece(bodyJSON.asString().c_str()),
                                       0, ERROR_INFO(error));
            REQUIRE(error.code == 0);
            REQUIRE(docs[i]);
        }
    }
    for (auto& coll : _collections) {
        REQUIRE(c4coll_getDocumentCount(coll) == 1);
    }

    // Push parameter
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    // Push to the remote
    replicate(replParams);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error error;
        for (auto& doc : docs) {
            doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
            REQUIRE(error.code == 0);
        }
    }
    // Verify docs are deleted
    for (size_t i = 0; i < _collectionCount; ++i) {
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
            for (size_t i = 0; i < _collectionCount; ++i) {
                C4Error error;
                docs[i] = c4coll_createDoc(_collections[i], slice(docID),
                                           json2fleece(bodyJSON.asString().c_str()),
                                           0, ERROR_INFO(error));
                REQUIRE(error.code == 0);
                REQUIRE(docs[i] != nullptr);
            }
        }
        for (auto coll : _collections) {
            REQUIRE(c4coll_getDocumentCount(coll) == 1);
        }
    }

    SECTION("Delete Doc") {
        deleteThenCreate = false;
    }

    // Perform Pull
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    for (size_t i = 0; i < _collectionCount; ++i) {
        C4Error error;
        c4::ref<C4Document> doc2 = c4coll_getDoc(_collections[i], slice(docID), true, kDocGetAll, ERROR_INFO(error));
        CHECK(error.code == 0);
        CHECK(doc2);
        if(deleteThenCreate) {
            CHECK(doc2->revID == docs[i]->revID);
            CHECK(c4coll_getDocumentCount(_collections[i]) == 1);
        } else {
            CHECK(doc2->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
            CHECK(c4coll_getDocumentCount(_collections[i]) == 0);
        }
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push Conflict SG", "[.SyncServerCollection]") {
    const string originalRevID = "1-3cb9cfb09f3f0b5142e618553966ab73539b8888";
    const string idPrefix = timePrefix();
    const string doc13ID = idPrefix + "0000013";

    initTest({ Roses, Tulips, Lavenders });

    for (auto coll : _collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 0, idPrefix);
    }

    updateDocIDs();

    // Push to remote
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replicate(replParams);

    // Update doc 13 on the remote
    string body = "{\"_rev\":\"" + originalRevID + "\",\"serverSideUpdate\":true}";
    for(auto& spec : _collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, doc13ID, slice(body), {}));
    }

    for(auto& coll : _collections) {
        // Create a conflict doc13 at local
        createRev(coll, slice(doc13ID), "2-f000"_sl, kFleeceBody);
        // Verify doc
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(doc13ID), true,
                                                kDocGetAll, nullptr);
        REQUIRE(doc);
        C4Slice revID = C4STR("2-f000");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    }


    C4Log("-------- Pushing Again (conflict) --------");
    _expectedDocPushErrors = {doc13ID};
    replicate(replParams);

    C4Log("-------- Pulling --------");
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replParams.setDocIDs(_docIDs);

    _expectedDocPushErrors = { };
    _expectedDocPullErrors = {doc13ID};
    replicate(replParams);

    C4Log("-------- Checking Conflict --------");
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(doc13ID), true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK((doc->flags & kDocConflicted) != 0);
        C4Slice revID = C4STR("2-f000");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
        REQUIRE(c4doc_selectCurrentRevision(doc));
        REQUIRE(c4doc_selectNextRevision(doc));
        revID = C4STR("2-883a2dacc15171a466f76b9d2c39669b");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull multiply-updated SG",
                 "[.SyncServerCollection]") {
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
    const string docID = idPrefix + "doc";

    initTest({ Roses, Tulips, Lavenders });

    for(auto& spec : _collectionSpecs) {
        _sg.upsertDoc(spec, docID + "?new_edits=false",
                      R"({"count":1, "_rev":"1-1111"})");
    }

    std::vector<std::unordered_map<alloc_slice, unsigned>> docIDs { _collectionCount };

    for(size_t i = 0; i < _collectionCount; ++i) {
        docIDs[i] = unordered_map<alloc_slice, unsigned> {{ alloc_slice(docID), 0 }};
    }

    ReplParams replParams { _collectionSpecs, kC4Disabled, kC4OneShot };
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    CHECK(_callbackStatus.progress.documentCount == _collectionCount);
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID),
                                                true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc);
        CHECK(doc->revID == "1-1111"_sl);
    }

    const std::array<std::string, 3> bodies {
            R"({"count":2, "_rev":"1-1111"})",
            R"({"count":3, "_rev":"2-c5557c751fcbfe4cd1f7221085d9ff70"})",
            R"({"count":4, "_rev":"3-2284e35327a3628df1ca8161edc78999"})"
    };

    for(auto& spec : _collectionSpecs) {
        for(const auto& body : bodies) {
            _sg.upsertDoc(spec, docID, body);
        }
    }

    replicate(replParams);
    for(auto& coll : _collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc);
        CHECK(doc->revID == "4-ffa3011c5ade4ec3a3ec5fe2296605ce"_sl);
    }
}
// This test takes > 1 minute per collection, so I have given it "SyncCollSlow" tag
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull iTunes deltas from Collection SG", "[.SyncCollSlow]") {
    string idPrefix = timePrefix() + "pidfsg";

    initTest({ Roses, Tulips, Lavenders });

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        for(auto& coll : _collections) { // Import 5000 docs per collection
            importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", coll, 0, false, 900, idPrefix);
        }
    };
    populateDB();

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams { _collectionSpecs, kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    for(int i = 0; i < _collectionCount; ++i) {
        auto numDocs = c4coll_getDocumentCount(_collections[i]);
        constexpr size_t docBufSize = 50;
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < numDocs; ++docNo) {
            char docID[docBufSize];
            snprintf(docID, docBufSize, "%s%07u", idPrefix.c_str(), docNo+1);
            C4Error error;
            c4::ref<C4Document> doc = c4coll_getDoc(_collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator it(props); it; ++it) {
                enc.writeKey(it.keyString());
                auto value = it.value();
                if (it.keyString() == "Play Count"_sl)
                    enc.writeInt(value.asInt() + 1);
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
    for(auto& coll : _collections) {
        numDocs += c4coll_getDocumentCount(coll);
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
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
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;
        // Verify docs
        int n = 0;
        for(auto& coll : _collections) {
            C4Error error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(coll, nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while (c4enum_next(e, ERROR_INFO(error))) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                auto revID = slice(info.revID);
                CHECK(revID.hasPrefix("2-"_sl));
                ++n;
            }
            CHECK(error.code == 0);
        }
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}
