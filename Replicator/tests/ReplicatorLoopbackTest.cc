//
//  ReplicatorLoopbackTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright 2017-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "ReplicatorLoopbackTest.hh"
#include "DBAccessTestWrapper.hh"
#include "Timer.hh"
#include "c4Database.hh"
#include "Base64.hh"
#include "betterassert.hh"
#include "fleece/Mutable.hh"
#include "fleece/PlatformCompat.hh"
#include <chrono>

using namespace litecore::actor;
using namespace std;

TEST_CASE("Options password logging redaction") {
    string          password("SEEKRIT");
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorAuthType));
    enc.writeString(kC4AuthTypeBasic);
    enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
    enc.writeString("emilio_lizardo");
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(password);
    enc.endDict();
    enc.endDict();
    alloc_slice         properties = enc.finish();
    Replicator::Options opts(kC4OneShot, kC4Disabled, properties);

    auto str = string(opts);
    Log("Options = %s", str.c_str());
    CHECK(str.find(password) == string::npos);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push replication from prebuilt database", "[Push]") {
    // Push a doc:
    createRev(_collDB1, "doc"_sl, kRevID, kEmptyFleeceBody);
    _expectedDocumentCount = 1;
    runPushReplication();

    // Use c4db_copyNamed to copy the db to a new file (with new UUIDs):
    C4Error     error;
    alloc_slice path(c4db_getPath(db));
    string      scratchDBName = format("scratch%" PRIms, chrono::milliseconds(time(nullptr)).count());
    REQUIRE(c4db_copyNamed(path, slice(scratchDBName), &dbConfig(), WITH_ERROR(&error)));

    // Open the copied db:
    c4db_release(db);
    db = c4db_openNamed(slice(scratchDBName), &dbConfig(), ERROR_INFO(error));
    REQUIRE(db);

    // Push from the copied db; this should reuse the checkpoint and not need to push any docs:
    _expectedUnitsComplete = 0;
    _expectedDocumentCount = 0;
    runPushReplication();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Fire Timer At Same Time", "[Push][Pull]") {
    atomic_int counter(0);
    Timer      t1([&counter] { counter++; });

    Timer t2([&counter] { counter++; });
    auto  at = chrono::steady_clock::now() + 500ms;
    t1.fireAt(at);
    t2.fireAt(at);

    REQUIRE_BEFORE(2s, counter == 2);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty DB", "[Push]") {
    runPushReplication();
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Small Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty Docs", "[Push]") {
    createRev(_collDB1, "doc"_sl, kRevID, kEmptyFleeceBody);
    _expectedDocumentCount = 1;

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large docs", "[Push]") {
    importJSONLines(sFixturesDir + "wikipedia_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push deletion", "[Push]") {
    createRev(_collDB1, "dok"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;
    runPushReplication();

    createNewRev(_collDB1, "dok"_sl, nullslice, kRevDeleted);
    Log("-------- Second Replication --------");
    runPushReplication();

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":2}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Second Replication --------");
    createRev(_collDB1, "new1"_sl, kRev1ID, kFleeceBody);
    createRev(_collDB1, "new2"_sl, kRev1ID_Alt, kFleeceBody);
    _expectedDocumentCount = 2;

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102}", "2-cc");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push 5000 Changes", "[Push]") {
    string revID;
    {
        TransactionHelper t(db);
        revID = createNewRev(_collDB1, "Doc"_sl, nullslice, kFleeceBody);
    }
    _expectedDocumentCount = 1;
    runPushReplication();

    Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for ( int i = 2; i <= 5000; ++i ) revID = createNewRev(_collDB1, "Doc"_sl, slice(revID), kFleeceBody);
    }

    Log("-------- Second Replication --------");
    runPushReplication();
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Resetting Checkpoint", "[Pull]") {
    createRev(_collDB1, "eenie"_sl, kRevID, kFleeceBody);
    createRev(_collDB1, "meenie"_sl, kRevID, kFleeceBody);
    createRev(_collDB1, "miney"_sl, kRevID, kFleeceBody);
    createRev(_collDB1, "moe"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 4;
    runPullReplication();

    {
        TransactionHelper t(db2);
        REQUIRE(c4coll_purgeDoc(_collDB2, "meenie"_sl, nullptr));
    }

    _expectedDocumentCount = 0;  // normal replication will not re-pull purged doc
    runPullReplication();

    _expectedDocumentCount = 1;  // resetting checkpoint does re-pull purged doc
    runReplicators(Replicator::Options::passive(_collSpec), Replicator::Options::pulling(kC4OneShot, _collSpec), true);

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB2, "meenie"_sl, true, kDocGetAll, nullptr);
    CHECK(doc != nullptr);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Push-Pull", "[Push][Pull]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);

    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushpull(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Second Replication --------");
    createNewRev(_collDB1, "0000001"_sl, kFleeceBody);
    createNewRev(_collDB1, "0000002"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    runReplicators(Replicator::Options::pushpull(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, R"({"local":102,"remote":100})", "2-cc");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large database", "[Push]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", _collDB1);
    _expectedDocumentCount = 12189;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":12189}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large database no-conflicts", "[Push][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();

    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", _collDB1);
    _expectedDocumentCount = 12189;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":12189}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull large database no-conflicts", "[Pull][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();

    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", _collDB1);
    _expectedDocumentCount = 12189;
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":12189}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Empty DB", "[Pull]") {
    runPullReplication();
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Small Non-Empty DB", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Pull", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");

    Log("-------- Second Replication --------");
    createRev(_collDB1, "new1"_sl, kRev1ID, kFleeceBody);
    createRev(_collDB1, "new2"_sl, kRev1ID_Alt, kFleeceBody);
    _expectedDocumentCount = 2;

    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":102}", "2-cc");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push/Pull Active Only", "[Pull]") {
    // Add 100 docs, then delete 50 of them:
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    constexpr size_t bufSize = 20;
    for ( unsigned i = 1; i <= 100; i += 2 ) {
        char docID[bufSize];
        snprintf(docID, bufSize, "%07u", i);
        createNewRev(_collDB1, slice(docID), nullslice, kRevDeleted);  // delete it
    }
    _expectedDocumentCount = 50;

    optional<Options> pushOpt, pullOpt;
    bool              pull = false, skipDeleted = false;

    SECTION("Pull") {
        // Pull replication. skipDeleted is automatic because destination is empty.
        pull = true;
        pullOpt.emplace(Replicator::Options::pulling(kC4OneShot, _collSpec));
        pushOpt.emplace(Replicator::Options::passive(_collSpec));
        skipDeleted = true;
        //pullOpt.setProperty(slice(kC4ReplicatorOptionSkipDeleted), "true"_sl);
    }
    SECTION("Push") {
        // Push replication. skipDeleted is not automatic, so test both ways:
        pushOpt.emplace(Replicator::Options::pushing(kC4OneShot, _collSpec));
        pullOpt.emplace(Replicator::Options::passive(_collSpec));
        SECTION("Push + SkipDeleted") {
            skipDeleted = true;
            pushOpt->setProperty(slice(kC4ReplicatorOptionSkipDeleted), "true"_sl);
        }
    }

    runReplicators(*pushOpt, *pullOpt);
    compareDatabases(false, false);

    if ( pull ) validateCheckpoints(db2, db, "{\"remote\":100}");
    else
        validateCheckpoints(db, db2, "{\"local\":100}");

    // If skipDeleted was used, ensure only 50 revisions got created (no tombstones):
    CHECK(c4coll_getLastSequence(_collDB2) == (skipDeleted ? 50 : 100));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push With Existing Key", "[Push]") {
    // Add a doc to db2; this adds the keys "name" and "gender" to the SharedKeys:
    {
        TransactionHelper t(db2);
        C4Error           c4err;
        alloc_slice       body = c4db_encodeJSON(db2, R"({"name":"obo", "gender":-7})"_sl, &c4err);
        REQUIRE(body.buf);
        createRev(_collDB2, "another"_sl, kRevID, body);
    }

    // Import names_100.json into db:
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;

    // Push db into db2:
    runPushReplication();
    compareDatabases(true);
    validateCheckpoints(db, db2, "{\"local\":100}");

    // Get one of the pushed docs from db2 and look up "gender":
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, "0000001"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    Dict  rev    = c4doc_getProperties(doc);
    Value gender = rev["gender"_sl];
    REQUIRE(gender != nullptr);
    REQUIRE(gender.asstring() == "female");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull existing revs", "[Pull]") {
    // Start with "mydoc" in both dbs with the same revs, so it won't be replicated.
    // But each db has one unique document.
    createRev(_collDB1, kDocID, kNonLocalRev1ID, kFleeceBody);
    createRev(_collDB1, kDocID, kNonLocalRev2ID, kFleeceBody);
    createRev(_collDB1, "onlyInDB1"_sl, kRevID, kFleeceBody);

    createRev(_collDB2, kDocID, kNonLocalRev1ID, kFleeceBody);
    createRev(_collDB2, kDocID, kNonLocalRev2ID, kFleeceBody);
    createRev(_collDB2, "onlyInDB2"_sl, kRevID, kFleeceBody);

    _expectedDocumentCount = 1;
    SECTION("Pull") { runPullReplication(); }
    SECTION("Push") { runPushReplication(); }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push expired doc", "[Pull]") {
    createRev(_collDB1, "obsolete"_sl, kNonLocalRev1ID, kFleeceBody);
    createRev(_collDB1, "fresh"_sl, kNonLocalRev1ID, kFleeceBody);
    createRev(_collDB1, "permanent"_sl, kNonLocalRev1ID, kFleeceBody);

    REQUIRE(c4coll_setDocExpiration(_collDB1, "obsolete"_sl, c4_now() - 1, nullptr));
    REQUIRE(c4coll_setDocExpiration(_collDB1, "fresh"_sl, c4_now() + 100000, nullptr));

    _expectedDocumentCount = 2;
    runPushReplication();

    // Verify that "obsolete" wasn't pushed, but the other two were:
    C4Error             error;
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, "obsolete"_sl, true, kDocGetAll, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);

    doc = c4coll_getDoc(_collDB1, "fresh"_sl, true, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc);
    CHECK(doc->revID == kNonLocalRev1ID);

    doc = c4coll_getDoc(_collDB1, "permanent"_sl, true, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc);
    CHECK(doc->revID == kNonLocalRev1ID);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull removed doc", "[Pull]") {
    {
        TransactionHelper t(db);
        // Start with "mydoc" in both dbs with the same revs
        createRev(_collDB1, kDocID, kRevID, kFleeceBody);
        createRev(_collDB2, kDocID, kRevID, kFleeceBody);

        // Add the "_removed" property. (Normally this is never added to a doc; it's just returned in
        // a fake revision body by the SG replictor, to indicate that the doc is removed from all
        // accessible channels.)
        fleece::SharedEncoder enc(c4db_getSharedFleeceEncoder(db));
        enc.beginDict();
        enc["_removed"_sl] = true;
        enc.endDict();
        createRev(_collDB1, kDocID, kRev2ID, enc.finish());
    }

    _expectedDocumentCount = 1;
    runPullReplication();

    // Verify the doc was purged:
    C4Error             error;
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB2, kDocID, true, kDocGetAll, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push To Erased Destination", "[Push]") {
    // Push; erase destination; push again. For #453
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runPushReplication();

    Log("--- Erasing db2, now pushing back to db...");
    deleteAndRecreateDB(db2);
    _collDB2 = createCollection(db2, _collSpec);

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Multiple Remotes", "[Push]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);
    SECTION("Default") {}
    SECTION("No-conflicts") { serverOpts.setNoIncomingConflicts(); }

    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");

    Log("--- Erasing db, now pushing back to db...");
    deleteAndRecreateDB();
    _collDB1 = createCollection(db, _collSpec);
    // Give the replication a unique ID so it won't know it's pushing to db again
    auto pushOpts = Replicator::Options::pushing(kC4OneShot, _collSpec);
    pushOpts.setProperty(C4STR(kC4ReplicatorOptionRemoteDBUniqueID), "three"_sl);
    runReplicators(serverOpts, pushOpts);
    validateCheckpoints(db2, db, "{\"local\":100}");
}

static Replicator::Options pushOptionsWithProperty(const char* property, const vector<string>& array,
                                                   C4CollectionSpec collSpec = kC4DefaultCollectionSpec) {
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey(slice(property));
    enc.beginArray();
    for ( const string& item : array ) enc << item;
    enc.endArray();
    enc.endDict();
    auto opts                         = Replicator::Options::pushing(kC4OneShot, collSpec);
    opts.collectionOpts[0].properties = AllocedDict(enc.finish());
    return opts;
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Different Checkpoint IDs", "[Push]") {
    // Test that replicators with different channel or docIDs options use different checkpoints
    // (#386)
    createFleeceRev(_collDB1, "doc"_sl, kRevID, R"({"agent":7})"_sl);
    _expectedDocumentCount = 1;

    runPushReplication();
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk1 = _checkpointIDs[0];

    _expectedDocumentCount = 0;  // because db2 already has the doc
    runReplicators(pushOptionsWithProperty(kC4ReplicatorOptionChannels, {"ABC", "CBS", "NBC"}, _collSpec),
                   Replicator::Options::passive(_collSpec));
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk2 = _checkpointIDs[0];
    CHECK(chk1 != chk2);

    runReplicators(pushOptionsWithProperty(kC4ReplicatorOptionDocIDs, {"wot's", "up", "doc"}, _collSpec),
                   Replicator::Options::passive(_collSpec));
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk3 = _checkpointIDs[0];
    CHECK(chk3 != chk2);
    CHECK(chk3 != chk1);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Overflowed Rev Tree", "[Push]") {
    // For #436
    if ( !isRevTrees() ) return;

    createRev(_collDB1, "doc"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;

    runPushReplication();

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, "doc"_sl, true, kDocGetAll, nullptr);
    alloc_slice         remote(c4doc_getRemoteAncestor(doc, 1));
    CHECK(remote == slice(kRevID));

    constexpr size_t bufSize = 32;

    for ( int gen = 2; gen <= 50; gen++ ) {
        char revID[bufSize];
        snprintf(revID, bufSize, "%d-0000", gen);
        createRev(_collDB1, "doc"_sl, slice(revID), kFleeceBody);
    }

    runPushReplication();

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":50}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Overflowed Rev Tree", "[Push]") {
    // For #436
    if ( !isRevTrees() ) return;

    createRev(_collDB1, "doc"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;

    runPullReplication();

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, "doc"_sl, true, kDocGetAll, nullptr);

    constexpr size_t bufSize = 32;

    for ( int gen = 2; gen <= 50; gen++ ) {
        char revID[bufSize];
        snprintf(revID, bufSize, "%d-0000", gen);
        createRev(_collDB1, "doc"_sl, slice(revID), kFleeceBody);
    }

    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":50}");

    // Check that doc is not conflicted in db2:
    doc = c4coll_getDoc(_collDB2, "doc"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "50-0000"_sl);
    CHECK(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
}

#pragma mark - CONTINUOUS:

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Of Tiny DB", "[Push][Continuous]") {
    createRev(_collDB1, "doc1"_sl, kRev1ID, kFleeceBody);
    createRev(_collDB1, "doc2"_sl, kRev1ID_Alt, kFleeceBody);
    _expectedDocumentCount = 2;

    stopWhenIdle();
    auto pushOpt = Replicator::Options::pushing(kC4Continuous, _collSpec);
    runReplicators(pushOpt, Replicator::Options::passive(_collSpec));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Of Tiny DB", "[Pull][Continuous]") {
    createRev(_collDB1, "doc1"_sl, kRev1ID, kFleeceBody);
    createRev(_collDB1, "doc2"_sl, kRev1ID_Alt, kFleeceBody);
    _expectedDocumentCount = 2;

    stopWhenIdle();
    auto pullOpt = Replicator::Options::pulling(kC4Continuous, _collSpec);
    runReplicators(Replicator::Options::passive(_collSpec), pullOpt);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Starting Empty", "[Push][Continuous]") {
    addDocsInParallel(1500ms, 6);
    runPushReplication(kC4Continuous);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push, Skip Purged", "[Push][Continuous]") {
    _parallelThread.reset(runInParallel([=]() {
        sleepFor(1s);
        {
            TransactionHelper t(db);
            createRev(_collDB1, c4str("docA"), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
            createRev(_collDB1, c4str("docB"), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
            bool ok = c4coll_purgeDoc(_collDB1, c4str("docA"), ERROR_INFO());
            REQUIRE(ok);
        }
        sleepFor(1s);  // give replicator a moment to detect the latest revs
        stopWhenIdle();
    }));
    // The purged document, namely "docA", should not be attempted by the push replicator.
    _expectedDocumentCount = 1;
    runPushReplication(kC4Continuous);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Revisions Starting Empty", "[Push][Continuous]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);
    //    SECTION("Default") {
    //    }
    //    SECTION("No-conflicts") {
    //        serverOpts.setNoIncomingConflicts();
    //    }
    SECTION("Pre-existing docs") {
        createRev(_collDB1, "doc1"_sl, kRev1ID, kFleeceBody);
        createRev(_collDB1, "doc2"_sl, kRev1ID, kFleeceBody);
        _expectedDocumentCount = 2;
        runPushReplication();
        C4Log("-------- Finished pre-existing push --------");
        createRev(_collDB2, "other1"_sl, kRev1ID, kFleeceBody);
    }
    addRevsInParallel(1000ms, alloc_slice("docko"), 1, 3);
    _expectedDocumentCount = 3;  // only 1 doc, but we get notified about it 3 times...
    runReplicators(Replicator::Options::pushing(kC4Continuous, _collSpec), serverOpts);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Starting Empty", "[Pull][Continuous]") {
    addDocsInParallel(1500ms, 6);
    runPullReplication(kC4Continuous);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push-Pull Starting Empty", "[Push][Pull][Continuous]") {
    addDocsInParallel(1500ms, 100);
    runPushPullReplication(kC4Continuous);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Fast Push", "[Push][Continuous]") {
    addDocsInParallel(100ms, 5000);
    runPushReplication(kC4Continuous);

    CHECK(c4coll_getDocumentCount(_collDB1) == c4coll_getDocumentCount(_collDB2));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Super-Fast Push", "[Push][Continuous]") {
    alloc_slice docID("dock");
    createRev(_collDB1, docID, kRev1ID, kFleeceBody);
    _expectedDocumentCount = -1;
    addRevsInParallel(10ms, docID, 2, 200);
    runPushReplication(kC4Continuous);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":201}");
}

#pragma mark - ATTACHMENTS:

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Attachments", "[Push][blob]") {
    vector<string>    attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
        _expectedDocsFinished.insert("att1");
    }

    auto opts            = Replicator::Options::pushing(kC4OneShot, _collSpec);
    _clientProgressLevel = kC4ReplProgressPerAttachment;
    runReplicators(opts, Replicator::Options::passive(_collSpec));

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");

    checkAttachments(db2, blobKeys, attachments);
    CHECK(_blobPushProgressCallbacks >= 2);
    CHECK(_blobPullProgressCallbacks == 0);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Attachments", "[Pull][blob]") {
    vector<string>    attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
        _expectedDocsFinished.insert("att1");
    }

    auto pullOpts        = Replicator::Options::pulling(kC4OneShot, _collSpec);
    auto serverOpts      = Replicator::Options::passive(_collSpec);
    _clientProgressLevel = _serverProgressLevel = kC4ReplProgressPerAttachment;
    runReplicators(serverOpts, pullOpts);

    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":1}");

    checkAttachments(db2, blobKeys, attachments);
    CHECK(_blobPushProgressCallbacks >= 2);
    CHECK(_blobPullProgressCallbacks >= 2);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Large Attachments", "[Pull][blob]") {
    string            att1(100000, '!');
    string            att2(80000, '?');
    string            att3(110000, '/');
    string            att4(3000, '.');
    vector<string>    attachments = {att1, att2, att3, att4};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":1}");

    checkAttachments(db2, blobKeys, attachments);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Lots Of Attachments", "[Pull][blob]") {
    static const int kNumDocs = 1000, kNumBlobsPerDoc = 5;
    Log("Creating %d docs, with %d blobs each ...", kNumDocs, kNumBlobsPerDoc);
    {
        // Create 10 docs, each with 1000 blobs:
        TransactionHelper t(db);
        constexpr size_t  docBufSize = 100, bodyBufSize = 100;
        char              docid[docBufSize], body[bodyBufSize];
        for ( int iDoc = 0; iDoc < kNumDocs; ++iDoc ) {
            //Log("Creating doc %3d ...", iDoc);
            vector<string> attachments;
            attachments.reserve(1000);
            for ( int iAtt = 0; iAtt < kNumBlobsPerDoc; iAtt++ ) {
                snprintf(body, bodyBufSize, "doc#%d attachment #%d", iDoc, iAtt);
                attachments.emplace_back(body);
            }
            snprintf(docid, docBufSize, "doc%03d", iDoc);
            addDocWithAttachments(db, _collSpec, c4str(docid), attachments, "text/plain");
            _expectedDocsFinished.insert(docid);
            ++_expectedDocumentCount;
        }
    }

    auto pullOpts        = Replicator::Options::pulling(kC4OneShot, _collSpec);
    _serverProgressLevel = kC4ReplProgressPerAttachment;
    runReplicators(Replicator::Options::passive(_collSpec), pullOpts);

    compareDatabases();

    validateCheckpoints(db2, db, format("{\"remote\":%d}", kNumDocs).c_str());
    CHECK(_blobPushProgressCallbacks == 0);
    CHECK(_blobPullProgressCallbacks >= kNumDocs * kNumBlobsPerDoc);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Uncompressible Blob", "[Push][blob]") {
    // Test case for issue #354
    alloc_slice       image       = readFile(sFixturesDir + "for#354.jpg");
    vector<string>    attachments = {string((const char*)image.buf, image.size)};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        // Use type text/plain so the replicator will try to compress the attachment
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");

    checkAttachments(db2, blobKeys, attachments);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Blobs Legacy Mode", "[Push][blob]") {
    vector<string>    attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }

    auto serverOpts = Replicator::Options::passive(_collSpec).setProperty("disable_blob_support"_sl, true);
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);

    checkAttachments(db2, blobKeys, attachments);

    string json = getDocJSON(_collDB2, "att1"_sl);
    replace(json, '"', '\'');
    if ( isRevTrees() ) {
        CHECK(json
              == "{'_attachments':{'blob_/attached/0':{'content_type':'text/"
                 "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27,'revpos':1,'stub':"
                 "true},"
                 "'blob_/attached/1':{'content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=','length':10,'revpos':1,'stub':true},"
                 "'blob_/attached/2':{'content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/"
                 "vlWAYkK/"
                 "YBwk=','length':0,'revpos':1,'stub':true}},"
                 "'attached':[{'@type':'blob','content_type':'text/"
                 "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27},"
                 "{'@type':'blob','content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=','length':10},"
                 "{'@type':'blob','content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                 "YBwk=','length':0}]}");
    } else {
        // (the only difference is that the 'revpos' properties are not present.)
        CHECK(json
              == "{'_attachments':{'blob_/attached/0':{'content_type':'text/"
                 "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27,'stub':"
                 "true},"
                 "'blob_/attached/1':{'content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=','length':10,'stub':true},"
                 "'blob_/attached/2':{'content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/"
                 "vlWAYkK/"
                 "YBwk=','length':0,'stub':true}},"
                 "'attached':[{'@type':'blob','content_type':'text/"
                 "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27},"
                 "{'@type':'blob','content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=','length':10},"
                 "{'@type':'blob','content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                 "YBwk=','length':0}]}");
    }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Blobs Legacy Mode", "[Push][blob]") {
    vector<string>    attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys               = addDocWithAttachments(db, _collSpec, "att1"_sl, attachments,
                                                       "text/plain");  //legacy
        _expectedDocumentCount = 1;
    }

    auto serverOpts = Replicator::Options::passive(_collSpec).setProperty("disable_blob_support"_sl, true);
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));

    checkAttachments(db2, blobKeys, attachments);
}

#pragma mark - FILTERS & VALIDATION:

TEST_CASE_METHOD(ReplicatorLoopbackTest, "DocID Filtered Replication", "[Push][Pull]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);

    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionDocIDs));
    enc.beginArray();
    enc.writeString("0000001"_sl);
    enc.writeString("0000010"_sl);
    enc.writeString("0000100"_sl);
    enc.endArray();
    enc.endDict();
    AllocedDict properties(enc.finish());

    SECTION("Push") {
        auto pushOptions                         = Replicator::Options::pushing(kC4OneShot, _collSpec);
        pushOptions.collectionOpts[0].properties = properties;
        _expectedDocumentCount                   = 3;
        runReplicators(pushOptions, Replicator::Options::passive(_collSpec));
    }
    SECTION("Pull") {
        auto pullOptions                         = Replicator::Options::pulling(kC4OneShot, _collSpec);
        pullOptions.collectionOpts[0].properties = properties;
        _expectedDocumentCount                   = 3;
        runReplicators(Replicator::Options::passive(_collSpec), pullOptions);
    }

    CHECK(c4coll_getDocumentCount(_collDB2) == 3);
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB2, "0000001"_sl, true, kDocGetAll, nullptr);
    CHECK(doc != nullptr);
    doc = c4coll_getDoc(_collDB2, "0000010"_sl, true, kDocGetAll, nullptr);
    CHECK(doc != nullptr);
    doc = c4coll_getDoc(_collDB2, "0000100"_sl, true, kDocGetAll, nullptr);
    CHECK(doc != nullptr);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Channels", "[Pull]") {
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey("filter"_sl);
    enc.writeString("Melitta"_sl);
    enc.endDict();
    alloc_slice data = enc.finish();
    auto        opts = Replicator::Options::pulling(kC4OneShot, _collSpec);
    opts.properties  = AllocedDict(data);

    // LiteCore's replicator doesn't support filters, so we expect an Unsupported error back:
    _expectedError = {LiteCoreDomain, kC4ErrorUnsupported};
    runReplicators(opts, Replicator::Options::passive(_collSpec));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Validation Failure", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    auto        pullOptions = Replicator::Options::passive(_collSpec);
    atomic<int> validationCount{0};
    pullOptions.collectionOpts[0].callbackContext = &validationCount;
    pullOptions.collectionOpts[0].pullFilter      = [](C4CollectionSpec collectionSpec, FLString docID, FLString revID,
                                                  C4RevisionFlags flags, FLDict body, void* context) -> bool {
        assert_always(flags == 0);  // can't use CHECK on a bg thread
        ++(*(atomic<int>*)context);
        return (Dict(body)["birthday"].asstring() < "1993");
    };
    _expectedDocPushErrors = set<string>{"0000052", "0000065", "0000071", "0000072"};
    _expectedDocumentCount = 100 - 4;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), pullOptions);
    validateCheckpoints(db, db2, "{\"local\":100}");

    // CBL-123: Change from == 100 to >= 100 to account for 403 getting
    // one retry before giving up
    CHECK(validationCount >= 100);
    CHECK(c4coll_getDocumentCount(_collDB2) == 96);
}

#pragma mark - CONFLICTS:

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Conflict", "[Push][Pull][Conflict]") {
    createFleeceRev(_collDB1, C4STR("conflict"), kNonLocalRev1ID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runPushReplication();
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(_collDB1, C4STR("conflict"), kConflictRev2AID, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, C4STR("conflict"), kConflictRev2BID, C4STR("{\"db\":2}"));

    if ( isRevTrees() ) {
        // Verify that rev 1 body is still available, for later use in conflict resolution:
        c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, C4STR("conflict"), true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == kConflictRev2AID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev1ID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    }

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"conflict"};
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));
    validateCheckpoints(db, db2, "{\"remote\":2}");

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, C4STR("conflict"), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
    CHECK(doc->selectedRev.revID == kConflictRev2AID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    if ( isRevTrees() ) {
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev1ID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    }
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
    CHECK(doc->selectedRev.revID == kConflictRev2BID);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(c4doc_getProperties(doc) != nullptr);
    if ( isRevTrees() ) {
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev1ID);
    }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Conflict", "[Push][Conflict][NoConflicts]") {
    // In the default no-outgoing-conflicts mode, make sure a local conflict isn't pushed to server:
    auto serverOpts = Replicator::Options::passive(_collSpec);
    createFleeceRev(_collDB1, C4STR("conflict"), kNonLocalRev1ID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(_collDB1, C4STR("conflict"), kConflictRev2AID, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, C4STR("conflict"), kConflictRev2BID, C4STR("{\"db\":2}"));
    REQUIRE(c4coll_getLastSequence(_collDB2) == 2);

    // Push db to db2 again:
    _expectedDocumentCount = 0;
    _expectedDocPushErrors = {"conflict"};
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    // Verify db2 didn't change:
    REQUIRE(c4coll_getLastSequence(_collDB2) == 2);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Conflict, NoIncomingConflicts", "[Push][Conflict][NoConflicts]") {
    // Put server in no-conflicts mode and verify that a conflict can't be pushed to it.
    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();
    createFleeceRev(_collDB1, C4STR("conflict"), kNonLocalRev1ID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(_collDB1, C4STR("conflict"), kConflictRev2AID, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, C4STR("conflict"), kConflictRev2BID, C4STR("{\"db\":2}"));
    REQUIRE(c4coll_getLastSequence(_collDB2) == 2);

    // Push db to db2 again:
    _expectedDocumentCount = 0;
    _expectedDocPushErrors = {"conflict"};
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    // Verify db2 didn't change:
    REQUIRE(c4coll_getLastSequence(_collDB2) == 2);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Then Push No-Conflicts", "[Pull][Push][Conflict][NoConflicts]") {
    static constexpr slice kTreeRevs[7] = {"", "1-1111", "2-2222", "3-3333", "4-4444", "5-5555", "6-6666"};
    static constexpr slice kVersions[7] = {"", "1@*", "2@*", "1@*", "2@*", "3@*", "4@*"};
    const slice*           kRevIDs      = isRevTrees() ? kTreeRevs : kVersions;

    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();

    createRev(_collDB1, kDocID, kRevIDs[1], kFleeceBody);
    createRev(_collDB1, kDocID, kRevIDs[2], kFleeceBody);
    _expectedDocumentCount = 1;

    Log("-------- First Replication db->db2 --------");
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));
    validateCheckpoints(db2, db, "{\"remote\":2}");

    Log("-------- Update Doc --------");
    alloc_slice body;
    {
        TransactionHelper t(db2);
        fleece::Encoder   enc(c4db_createFleeceEncoder(db2));
        enc.beginDict();
        enc.writeKey("answer"_sl);
        enc.writeInt(666);
        enc.endDict();
        body = enc.finish();
        createNewRev(_collDB2, kDocID, body);
        createNewRev(_collDB2, kDocID, body);
        _expectedDocumentCount = 1;
    }


    Log("-------- Second Replication db2->db --------");
    runReplicators(serverOpts, Replicator::Options::pushing(kC4OneShot, _collSpec));
    validateCheckpoints(db2, db, "{\"local\":3}");
    compareDatabases();

    Log("-------- Update Doc Again --------");
    createNewRev(_collDB2, kDocID, body);
    createNewRev(_collDB2, kDocID, body);
    _expectedDocumentCount = 1;

    Log("-------- Third Replication db2->db --------");
    runReplicators(serverOpts, Replicator::Options::pushing(kC4OneShot, _collSpec));
    validateCheckpoints(db2, db, "{\"local\":5}");
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Conflict Resolved Equivalently", "[Pull][Push][Conflict][NoConflicts]") {
    // CBL-726: Push conflict but server rev is just a newer ancestor of the local rev.
    // Local:  1-abcd -- 2-c001d00d -- 3-deadbeef -- 4-baba    (known remote rev: 2)
    // Server: 1-abcd -- 2-c001d00d -- 3-deadbeef
    // Pusher will fail with a 409 because the remote rev is too old.
    // When the puller sees the server has 3-deadbeef and updates the remote-rev, the puller
    // can retry and this time succeed.
    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();

    createRev(_collDB1, kDocID, kNonLocalRev1ID, kFleeceBody);
    createRev(_collDB1, kDocID, kNonLocalRev2ID, kFleeceBody);
    _expectedDocumentCount = 1;

    Log("-------- First Replication db<->db2 --------");
    runReplicators(Replicator::Options::pushpull(kC4OneShot, _collSpec), serverOpts);

    Log("-------- Update Doc --------");
    if ( isRevTrees() ) {
        createRev(_collDB1, kDocID, kRev3ID, kFleeceBody);
        createRev(_collDB1, kDocID, "4-baba"_sl, kFleeceBody);

        createRev(_collDB2, kDocID, kRev3ID, kFleeceBody);
    } else {
        createRev(_collDB1, kDocID, "1@DaveDaveDaveDaveDaveDA"_sl, kFleeceBody);
        createRev(_collDB1, kDocID, "1@*"_sl, kFleeceBody);

        createRev(_collDB2, kDocID, "1@DaveDaveDaveDaveDaveDA"_sl, kFleeceBody);
    }

    Log("-------- Second Replication db<->db2 --------");
    runReplicators(Replicator::Options::pushpull(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Lost Checkpoint No-Conflicts", "[Push][Conflict][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();

    createRev(_collDB1, kDocID, kRevID, kFleeceBody);
    createRev(_collDB1, kDocID, kRev2ID, kFleeceBody);

    Log("-------- First Replication: push db->db2 --------");
    _expectedDocumentCount = 1;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    clearCheckpoint(db, true);
    Log("-------- Second Replication: push db->db2 --------");
    _expectedDocumentCount = 0;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Lost Checkpoint Push after Delete", "[Push]") {
    auto serverOpts        = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();
    _ignoreLackOfDocErrors = true;
    _checkDocsFinished     = false;

    slice doc1_id = "doc1"_sl;
    slice doc2_id = "doc2"_sl;

    createRev(_collDB1, doc1_id, kRevID, kFleeceBody);
    createRev(_collDB1, doc2_id, kRevID, kFleeceBody);
    c4::ref<C4Document> doc1 = c4coll_getDoc(_collDB1, doc1_id, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc1);

    REQUIRE(c4coll_getDocumentCount(_collDB1) == 2);
    REQUIRE(c4coll_getDocumentCount(_collDB2) == 0);

    Log("-------- First Replication: push db->db2 --------");
    _expectedDocumentCount = 2;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);

    REQUIRE(c4coll_getDocumentCount(_collDB2) == 2);

    // delete doc1 from local
    {
        TransactionHelper t(db);
        // Delete the doc:
        c4::ref<C4Document> deletedDoc = c4doc_update(doc1, kC4SliceNull, kRevDeleted, ERROR_INFO());
        REQUIRE(deletedDoc);
        REQUIRE(deletedDoc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    }
    REQUIRE(c4coll_getDocumentCount(_collDB1) == 1);

    _expectedDocumentCount = 1;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    c4::ref<C4Document> doc1InDb2 = c4coll_getDoc(_collDB2, doc1_id, true, kDocGetMetadata, ERROR_INFO());
    CHECK((doc1InDb2 && (doc1InDb2->flags | kDocDeleted)));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incoming Deletion Conflict", "[Pull][Conflict]") {
    C4Slice docID = C4STR("Khan");

    createFleeceRev(_collDB1, docID, kRev1ID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runPushReplication();

    // Update doc in db, delete it in db2
    createFleeceRev(_collDB1, docID, kConflictRev2AID, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, docID, kConflictRev2BID, C4STR("{}"), kRevDeleted);

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"Khan"};
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kConflictRev2AID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == kConflictRev2BID);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    // Resolve the conflict in favor of the remote revision:
    {
        TransactionHelper t(db);
        C4Error           error;
        CHECK(c4doc_resolveConflict(doc, kConflictRev2BID, kConflictRev2AID, kC4SliceNull, kRevDeleted,
                                    WITH_ERROR(&error)));
        CHECK(c4doc_save(doc, 0, WITH_ERROR(&error)));
    }

    doc = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    CHECK(doc->revID == revOrVersID(kConflictRev2BID, "2@*"));

    // Update the doc and push it to db2:
    createNewRev(_collDB1, docID, kFleeceBody);
    C4Log("-------- Push db -> db2 --------");
    runPushReplication();

    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Local Deletion Conflict", "[Pull][Conflict]") {
    C4Slice docID = C4STR("Khan");

    createFleeceRev(_collDB1, docID, kRev1ID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runPushReplication();

    // Delete doc in db, update it in db2
    createFleeceRev(_collDB1, docID, kConflictRev2AID, C4STR("{}"), kRevDeleted);
    createFleeceRev(_collDB2, docID, kConflictRev2BID, C4STR("{\"db\":1}"));

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"Khan"};
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kConflictRev2AID);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == kConflictRev2BID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    // Resolve the conflict in favor of the remote revision:
    {
        TransactionHelper t(db);
        C4Error           error;
        CHECK(c4doc_resolveConflict(doc, kConflictRev2BID, kConflictRev2AID, kC4SliceNull, kRevDeleted,
                                    WITH_ERROR(&error)));
        CHECK(c4doc_save(doc, 0, WITH_ERROR(&error)));
    }

    doc = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    alloc_slice mergedID(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
    if ( isRevTrees() ) CHECK(mergedID == "2-2b2b2b2b,1-abcd"_sl);
    else
        CHECK(mergedID == "2@*, 1@MajorMajorMajorMajorQQ, 1@NorbertHeisenbergVonQQ;"_sl);

    // Update the doc and push it to db2:
    createNewRev(_collDB1, docID, kFleeceBody);
    runPushReplication();

    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Server Conflict Branch-Switch", "[Pull][Conflict]") {
    if ( !isRevTrees() ) return;  // this does not make sense with version vectors

    // For https://github.com/couchbase/sync_gateway/issues/3359
    C4Slice docID = C4STR("Khan");

    {
        TransactionHelper t(db);
        createRev(_collDB1, docID, C4STR("1-11111111"), kFleeceBody);
        createConflictingRev(_collDB1, docID, C4STR("1-11111111"), C4STR("2-22222222"));
        createConflictingRev(_collDB1, docID, C4STR("1-11111111"), C4STR("2-ffffffff"));
        createConflictingRev(_collDB1, docID, C4STR("2-22222222"), C4STR("3-33333333"));
    }
    _expectedDocumentCount = 1;
    runPullReplication();

    c4::ref<C4Document> doc = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    C4Slice revID = C4STR("3-33333333");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->flags & kDocConflicted) == 0);  // locally in db there is no conflict

    {
        TransactionHelper t(db);
        createConflictingRev(_collDB1, docID, C4STR("3-33333333"), C4STR("4-dddddddd"), kFleeceBody, kRevDeleted);
    }

    doc = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    revID = C4STR("2-ffffffff");
    CHECK(doc->revID == revID);
    CHECK(doc->selectedRev.revID == revID);

    SECTION("Unmodified") {
        Log("-------- Second pull --------");
        runPullReplication();

        doc = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->flags & kDocConflicted) == 0);
    }

    SECTION("Modify before 2nd pull") {
        {
            TransactionHelper t(db2);
            createRev(_collDB2, docID, C4STR("4-4444"), kC4SliceNull);
            _expectedDocPullErrors = {"Khan"};
        }

        Log("-------- Second pull --------");
        runPullReplication();

        doc = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK((doc->flags & kDocConflicted) != 0);
        revID = C4STR("4-4444");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->selectedRev.flags & kRevIsConflict) == 0);
        CHECK(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
        revID = C4STR("2-ffffffff");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

        {
            TransactionHelper t(db2);
            C4Error           error;
            CHECK(c4doc_resolveConflict(doc, C4STR("4-4444"), C4STR("2-ffffffff"), kC4SliceNull, 0,
                                        WITH_ERROR(&error)));
            CHECK(c4doc_save(doc, 0, WITH_ERROR(&error)));
        }

        doc = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK((doc->flags & kDocConflicted) == 0);
        revID = C4STR("4-4444");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(!c4doc_selectNextLeafRevision(doc, false, false, nullptr));
        CHECK(c4doc_selectParentRevision(doc));
        revID = C4STR("3-33333333");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_selectParentRevision(doc));
        revID = C4STR("2-22222222");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_selectParentRevision(doc));
        revID = C4STR("1-11111111");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(!c4doc_selectParentRevision(doc));
    }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push From Both Sides", "[Push][Continuous][Conflict]") {
    // temporarily disable it for VV
    if ( !isRevTrees() ) return;

    // NOTE: Despite the name, both sides are not active. Client pushes & pulls, server is passive.
    //       But both sides are rapidly changing the single document.
    alloc_slice docID("doc");
    auto        clientOpts = Replicator::Options::pushpull(kC4Continuous, _collSpec);
    _clientProgressLevel   = kC4ReplProgressPerDocument;
    auto serverOpts        = Replicator::Options::passive(_collSpec).setNoIncomingConflicts();
    installConflictHandler();

    static const int intervalMs = -500;  // random interval
    static const int iterations = 30;

    atomic_int         completed{0};
    unique_ptr<thread> thread1(runInParallel([&]() {
        addRevs(_collDB1, chrono::milliseconds(intervalMs), docID, 1, iterations, false, "db");
        if ( ++completed == 2 ) {
            sleepFor(1s);  // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }
    }));
    unique_ptr<thread> thread2(runInParallel([&]() {
        addRevs(_collDB2, chrono::milliseconds(intervalMs), docID, 1, iterations, false, "db2");
        if ( ++completed == 2 ) {
            sleepFor(1s);  // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }
    }));

    _expectedDocumentCount = -1;
    _expectedDocPushErrors = {"doc"};  // there are likely to be conflicts
    _ignoreLackOfDocErrors = true;     // ...but they may not occur
    _ignoreTransientErrors = true;     // (retries will show up as transient errors)
    _checkDocsFinished     = false;

    runReplicators(clientOpts, serverOpts);
    thread1->join();
    thread2->join();

    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Doc Notifications", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    for ( int i = 1; i <= 100; ++i ) _expectedDocsFinished.insert(format("%07d", i));
    auto opts            = Replicator::Options::pushing(kC4OneShot, _collSpec);
    _clientProgressLevel = kC4ReplProgressPerDocument;
    runReplicators(opts, Replicator::Options::passive(_collSpec));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Doc Notifications", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    for ( int i = 1; i <= 100; ++i ) _expectedDocsFinished.insert(format("%07d", i));
    auto opts            = Replicator::Options::pulling(kC4OneShot, _collSpec);
    _serverProgressLevel = kC4ReplProgressPerDocument;
    runReplicators(Replicator::Options::passive(_collSpec), opts);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "UnresolvedDocs", "[Push][Pull][Conflict]") {
    createFleeceRev(_collDB1, C4STR("conflict"), kRev1ID, C4STR("{}"));
    createFleeceRev(_collDB1, C4STR("non-conflict"), kRev1ID_Alt, C4STR("{}"));
    createFleeceRev(_collDB1, C4STR("db-deleted"), kRev1ID, C4STR("{}"));
    createFleeceRev(_collDB1, C4STR("db2-deleted"), kRev1ID, C4STR("{}"));
    _expectedDocumentCount = 4;

    // Push db to db2, so both will have docs:
    runPushReplication();

    // Update the docs differently in each db:
    createFleeceRev(_collDB1, C4STR("conflict"), revOrVersID("2-12121212", "1@ZegpoldZegpoldZegpoldA"),
                    C4STR("{\"db\": 1}"));
    createFleeceRev(_collDB2, C4STR("conflict"), revOrVersID("2-13131313", "1@BobBobBobBobBobBobBobA"),
                    C4STR("{\"db\": 2}"));
    createFleeceRev(_collDB1, C4STR("db-deleted"), revOrVersID("2-31313131", "1@ZegpoldZegpoldZegpoldA"),
                    C4STR("{\"db\":2}"), kRevDeleted);
    createFleeceRev(_collDB2, C4STR("db-deleted"), revOrVersID("2-32323232", "1@BobBobBobBobBobBobBobA"),
                    C4STR("{\"db\": 1}"));
    createFleeceRev(_collDB1, C4STR("db2-deleted"), revOrVersID("2-41414141", "1@ZegpoldZegpoldZegpoldA"),
                    C4STR("{\"db\": 1}"));
    createFleeceRev(_collDB2, C4STR("db2-deleted"), revOrVersID("2-42424242", "1@BobBobBobBobBobBobBobA"),
                    C4STR("{\"db\":2}"), kRevDeleted);

    // Now pull to db from db2, creating conflicts:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"conflict", "db-deleted", "db2-deleted"};
    _expectedDocumentCount = 3;
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));
    validateCheckpoints(db, db2, "{\"remote\":7}");

    auto e = DBAccessTestWrapper::unresolvedDocsEnumerator(_collDB1);
    REQUIRE(e);

    // verify only returns the conflicted documents, including the deleted ones.
    vector<C4Slice> docIDs   = {"conflict"_sl, "db-deleted"_sl, "db2-deleted"_sl};
    vector<C4Slice> revIDs   = {revOrVersID("2-12121212", "1@ZegpoldZegpoldZegpoldA"),
                                revOrVersID("2-31313131", "1@ZegpoldZegpoldZegpoldA"),
                                revOrVersID("2-41414141", "1@ZegpoldZegpoldZegpoldA")};
    vector<bool>    deleteds = {false, true, false};

    C4Error err;
    for ( int count = 0; count < 3; ++count ) {
        REQUIRE(c4enum_next(e, WITH_ERROR(&err)));
        C4DocumentInfo info;
        c4enum_getDocumentInfo(e, &info);
        CHECK(info.docID == docIDs[count]);
        CHECK(info.revID == revIDs[count]);
        CHECK((info.flags & kDocConflicted) == kDocConflicted);
        bool deleted = ((info.flags & kDocDeleted) != 0);
        CHECK(deleted == deleteds[count]);
    }
    CHECK(!c4enum_next(e, WITH_ERROR(&err)));
    c4enum_free(e);
}

#pragma mark - DELTA:

static void mutateDoc(C4Collection* collection, slice docID, const function<void(Dict, Encoder&)>& mutator) {
    C4Database*         db = c4coll_getDatabase(collection);
    TransactionHelper   t(db);
    C4Error             error;
    c4::ref<C4Document> doc = c4coll_getDoc(collection, docID, false, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc);
    Dict props = c4doc_getProperties(doc);

    Encoder enc(c4db_createFleeceEncoder(db));
    mutator(props, enc);
    alloc_slice newBody = enc.finish();

    C4String        history = doc->selectedRev.revID;
    C4DocPutRequest rq      = {};
    rq.body                 = newBody;
    rq.docID                = docID;
    rq.revFlags             = (doc->selectedRev.flags & kRevHasAttachments);
    rq.history              = &history;
    rq.historyCount         = 1;
    rq.save                 = true;
    doc                     = c4coll_putDoc(collection, &rq, nullptr, ERROR_INFO(error));
    CHECK(doc);
}

static void mutateDoc(C4Collection* collection, slice docID, function<void(MutableDict)> mutator) {
    mutateDoc(collection, docID, [&](Dict props, Encoder& enc) {
        MutableDict newProps = props.mutableCopy(kFLDeepCopyImmutables);
        mutator(newProps);
        enc.writeValue(newProps);
    });
}

static void mutationsForDelta(C4Collection* collection) {
    constexpr size_t bufSize = 20;
    for ( int i = 1; i <= 100; i += 7 ) {
        char docID[bufSize];
        snprintf(docID, bufSize, "%07u", i);
        mutateDoc(collection, slice(docID), [](MutableDict props) {
            props["birthday"_sl] = "1964-11-28"_sl;
            props["memberSince"_sl].remove();
            props["aNewProperty"_sl] = "!!!!";
        });
    }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Push+Push", "[Push][Delta]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);

    // Push db --> db2:
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Mutate Docs --------");
    mutationsForDelta(_collDB1);

    Log("-------- Second Push --------");
    atomic<int> validationCount{0};
    SECTION("No filter") {}
    SECTION("With filter") {
        Options::CollectionOptions& collOpts = serverOpts.collectionOpts[0];
        // Using a pull filter forces deltas to be applied earlier, before rev insertion.
        collOpts.callbackContext = &validationCount;
        collOpts.pullFilter = [](C4CollectionSpec collectionSpec, FLString docID, FLString revID, C4RevisionFlags flags,
                                 FLDict body, void* context) -> bool {
            assert_always(flags == 0);  // can't use CHECK on a bg thread
            ++(*(atomic<int>*)context);
            return true;
        };
    }

    _expectedDocumentCount = (100 + 6) / 7;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    CHECK(DBAccessTestWrapper::numDeltasApplied() - before == 15);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Bigger Delta Push+Push", "[Push][Delta]") {
    static constexpr int kNumDocs = 100, kNumProps = 1000;
    auto                 serverOpts = Replicator::Options::passive(_collSpec);

    // Push db --> db2:
    {
        TransactionHelper t(db);
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            string  docID = format("doc-%03d", docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(format("field%03d", p));
                enc.writeInt(RandomNumber());
            }
            enc.endDict();
            alloc_slice body = enc.finish();
            createNewRev(_collDB1, slice(docID), body);
        }
    }

    _expectedDocumentCount = kNumDocs;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();

    Log("-------- Mutate Docs --------");
    {
        TransactionHelper t(db);
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            string docID = format("doc-%03d", docNo);
            mutateDoc(_collDB1, slice(docID), [](Dict doc, Encoder& enc) {
                enc.beginDict();
                for ( Dict::iterator i(doc); i; ++i ) {
                    enc.writeKey(i.key());
                    auto value = i.value().asInt();
                    if ( RandomNumber() % 4 == 0 ) value = RandomNumber();
                    enc.writeInt(value);
                }
                enc.endDict();
            });
        }
    }

    Log("-------- Second Push --------");
    _expectedDocumentCount = kNumDocs;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    CHECK(DBAccessTestWrapper::numDeltasApplied() - before == kNumDocs);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Push+Pull", "[Push][Pull][Delta]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);

    // Push db --> db2:
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Mutate Docs In db2 --------");
    mutationsForDelta(_collDB2);

    Log("-------- Pull From db2 --------");
    _expectedDocumentCount = (100 + 6) / 7;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    if ( isRevTrees() )  // VV does not currently send deltas from a passive replicator
        CHECK(DBAccessTestWrapper::numDeltasApplied() - before == 15);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Attachments Push+Push", "[Push][Delta][blob]") {
    // Simulate SG which requires old-school "_attachments" property:
    auto serverOpts = Replicator::Options::passive(_collSpec).setProperty("disable_blob_support"_sl, true);

    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    {
        TransactionHelper t(db);
        addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    Log("-------- Push To db2 --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    Log("-------- Mutate Doc In db --------");
    bool modifiedDigest = false;
    SECTION("Not Modifying Digest") {
        // Modify attachment metadata (other than the digest):
        mutateDoc(_collDB1, "att1"_sl, [](MutableDict rev) {
            auto atts               = rev["attached"_sl].asArray().asMutable();
            auto blob               = atts[0].asDict().asMutable();
            blob["content_type"_sl] = "image/jpeg";
        });
    }
    SECTION("Modifying Digest") {
        // Simulate modifying an attachment, i.e. changing its "digest" property.
        // This goes through a different code path than other metadata changes; see comment in
        // IncomingRev::_handleRev()...
        // (In order to avoid having to save a new blob to the db, use same digest as 2nd blob.)
        mutateDoc(_collDB1, "att1"_sl, [](MutableDict rev) {
            auto atts               = rev["attached"_sl].asArray().asMutable();
            auto blob               = atts[0].asDict().asMutable();
            blob["digest"_sl]       = "sha1-rATs731fnP+PJv2Pm/WXWZsCw48=";
            blob["content_type"_sl] = "image/jpeg";
        });
        modifiedDigest = true;
    }

    Log("-------- Push To db2 Again --------");
    _expectedDocumentCount = 1;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    c4::ref<C4Document> doc2 = c4coll_getDoc(_collDB2, "att1"_sl, true, kDocGetAll, nullptr);
    alloc_slice         json = c4doc_bodyAsJSON(doc2, true, nullptr);

    int    expectedNumDeltas = 1;
    string expectedJson;
    if ( modifiedDigest ) {
        if ( isRevTrees() ) {
            // No delta used in this situation, as delta size *including modified revpos of each
            // attachment* > revisionSize * 1.2
            expectedNumDeltas = 0;
        }
        expectedJson = "{\"_attachments\":{\"blob_/attached/0\":{\"content_type\":\"image/"
                       "jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                       "WXWZsCw48=\",\"length\":27,\"revpos\":2,\"stub\":true},"
                       "\"blob_/attached/1\":{\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                       "WXWZsCw48=\",\"length\":10,\"revpos\":2,\"stub\":true},"
                       "\"blob_/attached/2\":{\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                       "YBwk=\",\"length\":0,\"revpos\":2,\"stub\":true}},"
                       "\"attached\":[{\"@type\":\"blob\",\"content_type\":\"image/"
                       "jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":27},"
                       "{\"@type\":\"blob\",\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                       "WXWZsCw48=\",\"length\":10},"
                       "{\"@type\":\"blob\",\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                       "YBwk=\",\"length\":0}]}";
    } else {
        expectedJson = "{\"_attachments\":{\"blob_/attached/0\":{\"content_type\":\"image/"
                       "jpeg\",\"digest\":\"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=\",\"length\":27,\"revpos\":"
                       "2,\"stub\":true},"
                       "\"blob_/attached/1\":{\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                       "WXWZsCw48=\",\"length\":10,\"revpos\":2,\"stub\":true},"
                       "\"blob_/attached/2\":{\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                       "YBwk=\",\"length\":0,\"revpos\":2,\"stub\":true}},"
                       "\"attached\":[{\"@type\":\"blob\",\"content_type\":\"image/"
                       "jpeg\",\"digest\":\"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=\",\"length\":27},"
                       "{\"@type\":\"blob\",\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                       "WXWZsCw48=\",\"length\":10},"
                       "{\"@type\":\"blob\",\"content_type\":\"text/"
                       "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                       "YBwk=\",\"length\":0}]}";
    }
    if ( !isRevTrees() ) replace(expectedJson, "\"revpos\":2,", "");  // With version vectors there's no revpos

    CHECK(DBAccessTestWrapper::numDeltasApplied() - before == expectedNumDeltas);
    CHECK(string(json) == expectedJson);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Attachments Pull+Pull", "[Pull][Delta][blob]") {
    // Simulate SG which requires old-school "_attachments" property:
    auto serverOpts = Replicator::Options::passive(_collSpec).setProperty("disable_blob_support"_sl, true);

    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    {
        TransactionHelper t(db);
        vector<string>    legacyNames{"attachment1", "attachment2", "attachment3"};
        addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain", &legacyNames, kRevKeepBody);
        _expectedDocumentCount = 1;
    }
    Log("-------- Pull To db2 --------");
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));
    validateCheckpoints(db2, db, "{\"remote\":1}");

    Log("-------- Mutate Doc In db --------");
    bool modifiedDigest = false;
    SECTION("Not Modifying Digest") {
        // Modify attachment metadata (other than the digest):
        mutateDoc(_collDB1, "att1"_sl, [](MutableDict rev) {
            auto atts               = rev["_attachments"_sl].asDict().asMutable();
            auto blob               = atts["attachment1"_sl].asDict().asMutable();
            blob["content_type"_sl] = "image/jpeg";
        });
    }
    SECTION("Not Modifying Digest") {
        // Simulate modifying an attachment, i.e. changing its "digest" property.
        // This goes through a different code path than other metadata changes; see comment in
        // IncomingRev::_handleRev()...
        // (In order to avoid having to save a new blob to the db, use same digest as 2nd blob.)
        mutateDoc(_collDB1, "att1"_sl, [](MutableDict rev) {
            auto atts               = rev["_attachments"_sl].asDict().asMutable();
            auto blob               = atts["attachment1"_sl].asDict().asMutable();
            blob["digest"_sl]       = "sha1-rATs731fnP+PJv2Pm/WXWZsCw48=";
            blob["content_type"_sl] = "image/jpeg";
        });
        modifiedDigest = true;
    }

    Log("-------- Pull To db2 Again --------");
    _expectedDocumentCount = 1;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot, _collSpec));
    if ( isRevTrees() )  // VV does not currently send deltas from a passive replicator
        CHECK(DBAccessTestWrapper::numDeltasApplied() - before == 1);

    c4::ref<C4Document> doc2 = c4coll_getDoc(_collDB2, "att1"_sl, true, kDocGetAll, nullptr);
    alloc_slice         json = c4doc_bodyAsJSON(doc2, true, nullptr);
    if ( modifiedDigest ) {
        CHECK(string(json)
              == "{\"_attachments\":{\"attachment1\":{\"content_type\":\"image/"
                 "jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":27},"
                 "\"attachment2\":{\"content_type\":\"text/"
                 "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=\",\"length\":10},"
                 "\"attachment3\":{\"content_type\":\"text/"
                 "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                 "YBwk=\",\"length\":0}}}");
    } else {
        CHECK(string(json)
              == "{\"_attachments\":{\"attachment1\":{\"content_type\":\"image/"
                 "jpeg\",\"digest\":\"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=\",\"length\":27},"
                 "\"attachment2\":{\"content_type\":\"text/"
                 "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
                 "WXWZsCw48=\",\"length\":10},"
                 "\"attachment3\":{\"content_type\":\"text/"
                 "plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/"
                 "YBwk=\",\"length\":0}}}");
    }
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Attachments Push+Pull", "[Push][Pull][Delta][blob]") {
    // Simulate SG which requires old-school "_attachments" property:
    auto serverOpts = Replicator::Options::passive(_collSpec).setProperty("disable_blob_support"_sl, true);

    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    {
        TransactionHelper t(db);
        addDocWithAttachments(db, _collSpec, "att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    Log("-------- Push Doc To db2 --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    Log("-------- Mutate Doc In db2 --------");
    // Simulate modifying an attachment. In order to avoid having to save a new blob to the db,
    // use the same digest as the 2nd blob.
    mutateDoc(_collDB2, "att1"_sl, [](MutableDict rev) {
        auto atts               = rev["_attachments"_sl].asDict().asMutable();
        auto blob               = atts["blob_/attached/0"_sl].asDict().asMutable();
        blob["digest"_sl]       = "sha1-rATs731fnP+PJv2Pm/WXWZsCw48=";
        blob["content_type"_sl] = "image/jpeg";
    });

    Log("-------- Pull From db2 --------");
    _expectedDocumentCount = 1;
    auto before            = DBAccessTestWrapper::numDeltasApplied();
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), serverOpts);
    if ( isRevTrees() )  // VV does not currently send deltas from a passive replicator
        CHECK(DBAccessTestWrapper::numDeltasApplied() - before == 1);

    c4::ref<C4Document> doc  = c4coll_getDoc(_collDB1, "att1"_sl, true, kDocGetAll, nullptr);
    alloc_slice         json = c4doc_bodyAsJSON(doc, true, nullptr);
    CHECK(string(json)
          == "{\"attached\":[{\"@type\":\"blob\",\"content_type\":\"image/"
             "jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
             "WXWZsCw48=\",\"length\":27},"
             "{\"@type\":\"blob\",\"content_type\":\"text/"
             "plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/"
             "WXWZsCw48=\",\"length\":10},"
             "{\"@type\":\"blob\",\"content_type\":\"text/plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/"
             "vlWAYkK/"
             "YBwk=\",\"length\":0}]}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull replication checkpoint mismatch", "[Pull]") {
    // CBSE-7341
    auto serverOpts = Replicator::Options::passive(_collSpec);

    // Push db --> db2:
    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    deleteAndRecreateDB(db2);
    _collDB2               = createCollection(db2, _collSpec);
    _expectedDocumentCount = 0;

    // This line causes a null deference SIGSEGV before the fix
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), serverOpts);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Resolve conflict with existing revision", "[Pull][Conflict]") {
    // CBL-1174
    createFleeceRev(_collDB1, C4STR("doc1"), kRev1ID, C4STR("{}"));
    createFleeceRev(_collDB1, C4STR("doc2"), kRev1ID_Alt, C4STR("{}"));
    _expectedDocumentCount = 2;
    runPushReplication();
    validateCheckpoints(db, db2, "{\"local\":2}");
    REQUIRE(c4coll_getLastSequence(_collDB1) == 2);
    REQUIRE(c4coll_getLastSequence(_collDB2) == 2);

    const slice kDoc1Rev2A = revOrVersID("2-1111111a", "1@AliceAliceAliceAliceAA");
    const slice kDoc1Rev2B = revOrVersID("2-1111111b", "1@BobBobBobBobBobBobBobA");
    const slice kDoc2Rev2A = revOrVersID("2-1111111a", "1@CarolCarolCarolCarolCA");
    const slice kDoc2Rev2B = revOrVersID("2-1111111b", "1@DaveDaveDaveDaveDaveDA");

    createFleeceRev(_collDB1, C4STR("doc1"), kDoc1Rev2A, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, C4STR("doc1"), kDoc1Rev2B, C4STR("{\"db\":2}"));
    createFleeceRev(_collDB1, C4STR("doc2"), kDoc2Rev2A, C4STR("{\"db\":1}"));
    createFleeceRev(_collDB2, C4STR("doc2"), kDoc2Rev2B, C4STR("{\"db\":2}"), kRevDeleted);
    REQUIRE(c4coll_getLastSequence(_collDB1) == 4);
    REQUIRE(c4coll_getLastSequence(_collDB2) == 4);

    _expectedDocPullErrors = set<string>{"doc1", "doc2"};
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));
    validateCheckpoints(db, db2, "{\"remote\":4}");
    if ( isRevTrees() )
        REQUIRE(c4coll_getLastSequence(_collDB1) == 6);  // #5(doc1) and #6(doc2) seq, received from other side
    REQUIRE(c4coll_getLastSequence(_collDB2) == 4);

    // resolve doc1 and create a new revision(#7) which should bring the `_lastSequence` greater than the doc2's sequence
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB1, C4STR("doc1"), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kDoc1Rev2A);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == kDoc1Rev2B);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    {
        TransactionHelper t(db);
        C4Error           error;
        CHECK(c4doc_resolveConflict(doc, kDoc1Rev2B, kDoc1Rev2A, json2fleece("{\"merged\":true}"), 0,
                                    WITH_ERROR(&error)));
        CHECK(c4doc_save(doc, 0, WITH_ERROR(&error)));
    }
    doc      = c4coll_getDoc(_collDB1, C4STR("doc1"), true, kDocGetAll, nullptr);
    auto seq = C4SequenceNumber(isRevTrees() ? 7 : 5);
    CHECK(doc->sequence == seq);
    CHECK(c4coll_getLastSequence(_collDB1) == seq);  // db-sequence is greater than #6(doc2)

    // resolve doc2; choose remote revision, so no need to create a new revision
    doc = c4coll_getDoc(_collDB1, C4STR("doc2"), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kDoc2Rev2A);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == kDoc2Rev2B);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    {
        TransactionHelper t(db);
        C4Error           error;
        CHECK(c4doc_resolveConflict(doc, kDoc2Rev2B, kDoc2Rev2A, kC4SliceNull, kRevDeleted, ERROR_INFO(&error)));
        CHECK(c4doc_save(doc, 0, WITH_ERROR(&error)));
    }

    doc = c4coll_getDoc(_collDB1, C4STR("doc2"), true, kDocGetAll, nullptr);
    CHECK(doc->revID == revOrVersID(kDoc1Rev2B, "3@*"));
    CHECK((doc->selectedRev.flags & kRevIsConflict) == 0);
    seq = C4SequenceNumber(isRevTrees() ? 8 : 6);
    CHECK(doc->sequence == seq);
    CHECK(c4coll_getLastSequence(_collDB1) == seq);
}

#pragma mark - PROPERTY ENCRYPTION:

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Encrypted Properties No Callback", "[Push][Sync][Encryption]") {
    {
        TransactionHelper t(db);
        createFleeceRev(_collDB1, "seekrit"_sl, kRevID, R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"_sl);
    }

    _expectedDocumentCount   = 0;
    _expectedDocPushErrors   = {"seekrit"};
    auto                opts = Replicator::Options::pushing(kC4OneShot, _collSpec);
    ExpectingExceptions x;
    runReplicators(opts, Replicator::Options::passive(_collSpec));
    auto defaultColl = db2->getDefaultCollection();
    REQUIRE(defaultColl->getDocumentCount() == 0);
}


#ifdef COUCHBASE_ENTERPRISE

struct TestEncryptorContext {
    slice docID;
    slice keyPath;
    bool  called{};
};

static C4SliceResult testEncryptor(void* rawCtx, C4CollectionSpec collection, C4String documentID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError) {
    auto context    = (TestEncryptorContext*)rawCtx;
    context->called = true;
    CHECK(documentID == context->docID);
    CHECK(keyPath == context->keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

static C4SliceResult testDecryptor(void* rawCtx, C4CollectionSpec collection, C4String documentID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm, C4String keyID,
                                   C4Error* outError) {
    auto context    = (TestEncryptorContext*)rawCtx;
    context->called = true;
    CHECK(documentID == context->docID);
    CHECK(keyPath == context->keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Replicate Encrypted Properties", "[Push][Pull][Sync][Encryption]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));

    slice originalJSON = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    {
        TransactionHelper t(db);
        createFleeceRev(_collDB1, "seekrit"_sl, kRevID, originalJSON);
        _expectedDocumentCount = 1;
    }

    TestEncryptorContext encryptContext = {"seekrit", "SSN", false};
    TestEncryptorContext decryptContext = {"seekrit", "SSN", false};

    auto opts              = Replicator::Options::pushing(kC4OneShot, _collSpec);
    opts.propertyEncryptor = &testEncryptor;
    opts.propertyDecryptor = &testDecryptor;
    opts.callbackContext   = &encryptContext;

    auto serverOpts              = Replicator::Options::passive(_collSpec);
    serverOpts.propertyEncryptor = &testEncryptor;
    serverOpts.propertyDecryptor = &testDecryptor;
    serverOpts.callbackContext   = &decryptContext;
    if ( !TestDecryption ) serverOpts.setNoPropertyDecryption();

    runReplicators(opts, serverOpts);

    // Verify the synced document in db2:
    CHECK(encryptContext.called);
    c4::ref<C4Document> doc = c4coll_getDoc(_collDB2, "seekrit"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    Dict props = c4doc_getProperties(doc);

    if ( TestDecryption ) {
        CHECK(props.toJSON(false, true) == originalJSON);
    } else {
        CHECK(props.toJSON(false, true)
              == R"({"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

        // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
        // so the property won't be converted back from the server schema.
        slice       cipherb64 = props["encrypted$SSN"].asDict()["ciphertext"].asString();
        auto        cipher    = base64::decode(cipherb64);
        alloc_slice clear     = UnbreakableEncryption(cipher, -1);
        CHECK(clear == "\"123-45-6789\"");
    }
}
#endif  // COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Replication Collections Must Match", "[Push][Pull][Sync]") {
    Options opts       = GENERATE_COPY(Options::pushing(kC4OneShot, _collSpec), Options::pulling(kC4OneShot, _collSpec),
                                       Options::pushpull(kC4OneShot, _collSpec));
    Options serverOpts = Options::passive(_collSpec);

    Retained<C4Collection>     coll = createCollection(db, {"foo"_sl, "bar"_sl});
    Options::CollectionOptions tmp{{"foo"_sl, "bar"_sl}};
    tmp.pull = opts.pull(0);
    tmp.push = opts.push(0);
    opts.collectionOpts.push_back(tmp);

    SECTION("Mismatched count should return NotFound") {
        // No-op
    }

    SECTION("Same count but mismatched names should return NotFound") {
        tmp      = Options::CollectionOptions({"foo"_sl, "baz"_sl});
        tmp.pull = kC4Passive;
        tmp.push = kC4Passive;
        serverOpts.collectionOpts.insert(serverOpts.collectionOpts.begin(), tmp);
    }

    _expectedError.domain = WebSocketDomain;
    _expectedError.code   = 404;
    runReplicators(opts, serverOpts);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Conflict Includes Rev", "[Push][Sync]") {
    // The new push property, "conflictIncludesRev", introduced by the resolution of CBL-2637,
    // also fixed the scenrio of CBL-127.

    FLSlice docID = C4STR("doc");
    slice   jBody = R"({"name":"otherDB"})"_sl;
    auto    revID = createFleeceRev(_collDB2, docID, nullslice, jBody);
    if ( isRevTrees() ) { REQUIRE(c4rev_getGeneration(slice(revID)) == 1); }

    _expectedDocumentCount = 1;
    // Pre-conditions: db is empty, db2 has one doc.
    runPushPullReplication();

    // Post-conditions: db and db2 are sync'ed.
    c4::ref<C4Document> docInDb1 = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    c4::ref<C4Document> docInDb2 = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
    string              revInDb1(alloc_slice(c4doc_getSelectedRevIDGlobalForm(docInDb1)));
    string              revInDb2(alloc_slice(c4doc_getSelectedRevIDGlobalForm(docInDb2)));
    REQUIRE(revInDb1 == revInDb2);
    REQUIRE(revID == string(docInDb2->revID));

    // Modify the document in db
    slice modifiedBody = R"({"name":"otherDB","modified":1})"_sl;
    auto  revID_2      = createFleeceRev(_collDB1, docID, nullslice, modifiedBody);
    if ( isRevTrees() ) { CHECK(c4rev_getGeneration(slice(revID_2)) == 2); }

    Replicator::Options serverOpts = Replicator::Options::passive(_collSpec);
    Replicator::Options clientOpts = Replicator::Options::pushing(kC4OneShot, _collSpec);

    SECTION("Same Target Revision 1 Was Synced") {}
    SECTION("Assign a New UID to the Target") {
        // We are to push revision 2 but with different UID, the pusher lost track of the
        // remote counterpart of revision 1. The property "conflictIncludesRev" attached to the
        // "proposeChange" message helps to resolve it.
        clientOpts.setProperty(kC4ReplicatorOptionRemoteDBUniqueID, "DifferentUID"_sl);
    }

    _expectedDocumentCount = 1;
    runReplicators(clientOpts, serverOpts);
    docInDb1 = c4coll_getDoc(_collDB1, docID, true, kDocGetAll, nullptr);
    docInDb2 = c4coll_getDoc(_collDB2, docID, true, kDocGetAll, nullptr);
    revInDb1 = alloc_slice(c4doc_getSelectedRevIDGlobalForm(docInDb1));
    revInDb2 = alloc_slice(c4doc_getSelectedRevIDGlobalForm(docInDb2));
    CHECK(revInDb1 == revInDb2);
    REQUIRE(revID_2 == string(docInDb1->revID));
}
