//
//  ReplicatorLoopbackTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplicatorLoopbackTest.hh"
#include "Worker.hh"
#include "DBAccess.hh"
#include "Timer.hh"
#include "Database.hh"
#include "PrebuiltCopier.hh"
#include <chrono>
#include "betterassert.hh"
#include "fleece/Mutable.hh"

using namespace litecore::actor;

constexpr duration ReplicatorLoopbackTest::kLatency;

TEST_CASE("Options password logging redaction") {
    string password("SEEKRIT");
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
    alloc_slice properties = enc.finish();
    Replicator::Options opts(kC4OneShot, kC4Disabled, properties);

    auto str = string(opts);
    Log("Options = %s", str.c_str());
    CHECK(str.find(password) == string::npos);
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push replication from prebuilt database", "[Push]") {
    createRev("doc"_sl, kRevID, kEmptyFleeceBody);
    _expectedDocumentCount = 1;
    runPushReplication();

    // No more progress should be made with the imported DB
    _expectedUnitsComplete = 0;
    _expectedDocumentCount = 0;

    FilePath original = db->path();
    FilePath newPath = FilePath::tempDirectory()["scratch.cblite2/"];
    string newPathStr = newPath.path();
    CopyPrebuiltDB(original, newPath, &db->config);
    C4DatabaseConfig config = db->config;
    c4db_free(db);
    db = c4db_open(c4str(newPathStr.c_str()), &config, nullptr);
    runPushReplication();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Fire Timer At Same Time", "[Push][Pull]") {
    atomic_int counter(0);
    Timer t1([&counter] { 
        counter++; 
    });

    Timer t2([&counter] {
        counter++;
    });
    auto at = chrono::steady_clock::now() + chrono::milliseconds(500);
    t1.fireAt(at);
    t2.fireAt(at);

    this_thread::sleep_for(chrono::milliseconds(600));
    CHECK(counter == 2);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty DB", "[Push]") {
    runPushReplication();
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Small Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty Docs", "[Push]") {
    createRev("doc"_sl, kRevID, kEmptyFleeceBody);
    _expectedDocumentCount = 1;

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large docs", "[Push]") {
    importJSONLines(sFixturesDir + "wikipedia_100.json");
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push deletion", "[Push]") {
    createRev("dok"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;
    runPushReplication();

    createNewRev(db, "dok"_sl, nullslice, kRevDeleted);
    Log("-------- Second Replication --------");
    runPushReplication();

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":2}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Second Replication --------");
    createRev("new1"_sl, "1-1234"_sl, kFleeceBody);
    createRev("new2"_sl, "1-2341"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102}", "2-cc");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push 5000 Changes", "[Push]") {
    string revID;
    {
        TransactionHelper t(db);
        revID = createNewRev(db, "Doc"_sl, nullslice, kFleeceBody);
    }
    _expectedDocumentCount = 1;
    runPushReplication();

    Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for (int i = 2; i <= 5000; ++i)
            revID = createNewRev(db, "Doc"_sl, slice(revID), kFleeceBody);
    }

    Log("-------- Second Replication --------");
    runPushReplication();
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Resetting Checkpoint", "[Pull]") {
    createRev("eenie"_sl, kRevID, kFleeceBody);
    createRev("meenie"_sl, kRevID, kFleeceBody);
    createRev("miney"_sl, kRevID, kFleeceBody);
    createRev("moe"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 4;
    runPullReplication();

    {
        TransactionHelper t(db2);
        REQUIRE(c4db_purgeDoc(db2, "meenie"_sl, nullptr));
    }
    
    _expectedDocumentCount = 0;  // normal replication will not re-pull purged doc
    runPullReplication();

    auto pullOpts = Replicator::Options::pulling();
    pullOpts.setProperty(slice(kC4ReplicatorResetCheckpoint), true);
    _expectedDocumentCount = 1; // resetting checkpoint does re-pull purged doc
    runReplicators(Replicator::Options::passive(), pullOpts);

    c4::ref<C4Document> doc = c4doc_get(db2, "meenie"_sl, true, nullptr);
    CHECK(doc != nullptr);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Push-Pull", "[Push][Pull]") {
    auto serverOpts = Replicator::Options::passive();

    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options(kC4OneShot, kC4OneShot), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Second Replication --------");
    createRev("0000001"_sl, kRev2ID, kFleeceBody);
    createRev("0000002"_sl, kRev2ID, kFleeceBody);
    _expectedDocumentCount = 2;

    runReplicators(Replicator::Options(kC4OneShot, kC4OneShot), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102,\"remote\":100}", "2-cc");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large database", "[Push]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    _expectedDocumentCount = 12189;
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":12189}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push large database no-conflicts", "[Push][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();

    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    _expectedDocumentCount = 12189;
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":12189}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull large database no-conflicts", "[Pull][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();

    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    _expectedDocumentCount = 12189;
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot));
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":12189}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Empty DB", "[Pull]") {
    runPullReplication();
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Small Non-Empty DB", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Pull", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");

    Log("-------- Second Replication --------");
    createRev("new1"_sl, "1-2341"_sl, kFleeceBody);
    createRev("new2"_sl, "1-2341"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":102}", "2-cc");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push/Pull Active Only", "[Pull]") {
    // Add 100 docs, then delete 50 of them:
    importJSONLines(sFixturesDir + "names_100.json");
    for (unsigned i = 1; i <= 100; i += 2) {
        char docID[20];
        sprintf(docID, "%07u", i);
        createRev(slice(docID), kRev2ID, nullslice, kRevDeleted); // delete it
    }
    _expectedDocumentCount = 50;

    auto pushOpt = Replicator::Options::passive();
    auto pullOpt = Replicator::Options::passive();
    bool pull = false, skipDeleted = false;

    SECTION("Pull") {
        // Pull replication. skipDeleted is automatic because destination is empty.
        pull = true;
        pullOpt = Replicator::Options::pulling();
        skipDeleted = true;
        //pullOpt.setProperty(slice(kC4ReplicatorOptionSkipDeleted), "true"_sl);
    }
    SECTION("Push") {
        // Push replication. skipDeleted is not automatic, so test both ways:
        pushOpt = Replicator::Options::pushing();
        SECTION("Push + SkipDeleted") {
            skipDeleted = true;
            pushOpt.setProperty(slice(kC4ReplicatorOptionSkipDeleted), "true"_sl);
        }
    }

    runReplicators(pushOpt, pullOpt);
    compareDatabases(false, false);

    if (pull)
        validateCheckpoints(db2, db, "{\"remote\":100}");
    else
        validateCheckpoints(db, db2, "{\"local\":100}");

    // If skipDeleted was used, ensure only 50 revisions got created (no tombstones):
    CHECK(c4db_getLastSequence(db2) == (skipDeleted ?50 : 100));
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push With Existing Key", "[Push]") {
    // Add a doc to db2; this adds the keys "name" and "gender" to the SharedKeys:
    {
        TransactionHelper t(db2);
        C4Error c4err;
        alloc_slice body = c4db_encodeJSON(db2, "{\"name\":\"obo\", \"gender\":-7}"_sl, &c4err);
        REQUIRE(body.buf);
        createRev(db2, "another"_sl, kRevID, body);
    }

    // Import names_100.json into db:
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;

    // Push db into db2:
    runPushReplication();
    compareDatabases(true);
    validateCheckpoints(db, db2, "{\"local\":100}");

    // Get one of the pushed docs from db2 and look up "gender":
    c4::ref<C4Document> doc = c4doc_get(db2, "0000001"_sl, true, nullptr);
    REQUIRE(doc);
    Doc rev = c4doc_createFleeceDoc(doc);
    Value gender = rev["gender"_sl];
    REQUIRE(gender != nullptr);
    REQUIRE(gender.asstring() == "female");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull existing revs", "[Pull]") {
    // Start with "mydoc" in both dbs with the same revs, so it won't be replicated.
    // But each db has one unique document.
    createRev(db, kDocID, kRevID, kFleeceBody);
    createRev(db, kDocID, kRev2ID, kFleeceBody);
    createRev(db, "onlyInDB1"_sl, kRevID, kFleeceBody);
    
    createRev(db2, kDocID, kRevID, kFleeceBody);
    createRev(db2, kDocID, kRev2ID, kFleeceBody);
    createRev(db2, "onlyInDB2"_sl, kRevID, kFleeceBody);

    _expectedDocumentCount = 1;
    SECTION("Pull") {
        runPullReplication();
    }
    SECTION("Push") {
        runPushReplication();
    }
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push expired doc", "[Pull]") {
    createRev(db, "obsolete"_sl, kRevID, kFleeceBody);
    createRev(db, "fresh"_sl, kRevID, kFleeceBody);
    createRev(db, "permanent"_sl, kRevID, kFleeceBody);

    REQUIRE(c4doc_setExpiration(db, "obsolete"_sl, c4_now() - 1, nullptr));
    REQUIRE(c4doc_setExpiration(db, "fresh"_sl, c4_now() + 100000, nullptr));

    _expectedDocumentCount = 2;
    runPushReplication();

    // Verify that "obsolete" wasn't pushed, but the other two were:
    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db2, "obsolete"_sl, true, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);

    doc = c4doc_get(db2, "fresh"_sl, true, &error);
    REQUIRE(doc);
    CHECK(doc->revID == kRevID);

    doc = c4doc_get(db2, "permanent"_sl, true, &error);
    REQUIRE(doc);
    CHECK(doc->revID == kRevID);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull removed doc", "[Pull]") {
    {
        TransactionHelper t(db);
        // Start with "mydoc" in both dbs with the same revs
        createRev(db, kDocID, kRevID, kFleeceBody);
        createRev(db2, kDocID, kRevID, kFleeceBody);

        // Add the "_removed" property. (Normally this is never added to a doc; it's just returned in
        // a fake revision body by the SG replictor, to indicate that the doc is removed from all
        // accessible channels.)
        fleece::SharedEncoder enc(c4db_getSharedFleeceEncoder(db));
        enc.beginDict();
        enc["_removed"_sl] = true;
        enc.endDict();
        createRev(db, kDocID, kRev2ID, enc.finish());
    }

    _expectedDocumentCount = 1;
    runPullReplication();

    // Verify the doc was purged:
    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db2, kDocID, true, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push To Erased Destination", "[Push]") {
    // Push; erase destination; push again. For #453
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runPushReplication();

    Log("--- Erasing db2, now pushing back to db...");
    deleteAndRecreateDB(db2);

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Multiple Remotes", "[Push]") {
    auto serverOpts = Replicator::Options::passive();
    SECTION("Default") {
    }
    SECTION("No-conflicts") {
        serverOpts.setNoIncomingConflicts();
    }

    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runReplicators(serverOpts, Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");

    Log("--- Erasing db, now pushing back to db...");
    deleteAndRecreateDB();
    // Give the replication a unique ID so it won't know it's pushing to db again
    auto pushOpts = Replicator::Options::pushing();
    pushOpts.setProperty(C4STR(kC4ReplicatorOptionRemoteDBUniqueID), "three"_sl);
    runReplicators(serverOpts, pushOpts);
    validateCheckpoints(db2, db, "{\"local\":100}");
}


static Replicator::Options pushOptionsWithProperty(const char *property, vector<string> array) {
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey(slice(property));
    enc.beginArray();
    for (const string &item : array)
        enc << item;
    enc.endArray();
    enc.endDict();
    auto opts = Replicator::Options::pushing();
    opts.properties = AllocedDict(enc.finish());
    return opts;
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Different Checkpoint IDs", "[Push]") {
    // Test that replicators with different channel or docIDs options use different checkpoints
    // (#386)
    createFleeceRev(db, "doc"_sl, kRevID, "{\"agent\":7}"_sl);
    _expectedDocumentCount = 1;

    runPushReplication();
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk1 = _checkpointID;

    _expectedDocumentCount = 0;     // because db2 already has the doc
    runReplicators(pushOptionsWithProperty(kC4ReplicatorOptionChannels, {"ABC", "CBS", "NBC"}),
                   Replicator::Options::passive());
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk2 = _checkpointID;
    CHECK(chk1 != chk2);

    runReplicators(pushOptionsWithProperty(kC4ReplicatorOptionDocIDs, {"wot's", "up", "doc"}),
                   Replicator::Options::passive());
    validateCheckpoints(db, db2, "{\"local\":1}");
    alloc_slice chk3 = _checkpointID;
    CHECK(chk3 != chk2);
    CHECK(chk3 != chk1);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Overflowed Rev Tree", "[Push]") {
    // For #436
    createRev("doc"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;

    runPushReplication();

    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
    alloc_slice remote(c4doc_getRemoteAncestor(doc, 1));
    CHECK(remote == slice(kRevID));

    for (int gen = 2; gen <= 50; gen++) {
        char revID[32];
        sprintf(revID, "%d-0000", gen);
        createRev("doc"_sl, slice(revID), kFleeceBody);
    }

    runPushReplication();

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":50}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Overflowed Rev Tree", "[Push]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    _expectedDocumentCount = 1;

    runPullReplication();

    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);

    for (int gen = 2; gen <= 50; gen++) {
        char revID[32];
        sprintf(revID, "%d-0000", gen);
        createRev("doc"_sl, slice(revID), kFleeceBody);
    }

    runPullReplication();
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":50}");

    // Check that doc is not conflicted in db2:
    doc = c4doc_get(db2, "doc"_sl, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "50-0000"_sl);
    CHECK(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
}


#pragma mark - CONTINUOUS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Of Tiny DB", "[Push][Continuous]") {
    createRev(db, "doc1"_sl, "1-11"_sl, kFleeceBody);
    createRev(db, "doc2"_sl, "1-aa"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    stopWhenIdle();
    auto pushOpt = Replicator::Options::pushing(kC4Continuous);
    runReplicators(pushOpt, Replicator::Options::passive());
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Of Tiny DB", "[Pull][Continuous]") {
    createRev(db, "doc1"_sl, "1-11"_sl, kFleeceBody);
    createRev(db, "doc2"_sl, "1-aa"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    stopWhenIdle();
    auto pullOpt = Replicator::Options::pulling(kC4Continuous);
    runReplicators(Replicator::Options::passive(), pullOpt);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Starting Empty", "[Push][Continuous]") {
    addDocsInParallel(chrono::milliseconds(1500), 6);
    runPushReplication(kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Revisions Starting Empty", "[Push][Continuous]") {
    auto serverOpts = Replicator::Options::passive();
    SECTION("Default") {
    }
    SECTION("No-conflicts") {
        serverOpts.setNoIncomingConflicts();
    }
    addRevsInParallel(chrono::milliseconds(1000), alloc_slice("docko"), 1, 3);
    _expectedDocumentCount = 3; // only 1 doc, but we get notified about it 3 times...
    runReplicators(Replicator::Options::pushing(kC4Continuous), serverOpts);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Starting Empty", "[Pull][Continuous]") {
    addDocsInParallel(chrono::milliseconds(1500), 6);
    runPullReplication(kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push-Pull Starting Empty", "[Push][Pull][Continuous]") {
    addDocsInParallel(chrono::milliseconds(1500), 100);
    runPushPullReplication(kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Fast Push", "[Push][Continuous]") {
    addDocsInParallel(chrono::milliseconds(100), 5000);
    runPushReplication(kC4Continuous);

	CHECK(c4db_getDocumentCount(db) == c4db_getDocumentCount(db2));
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Super-Fast Push", "[Push][Continuous]") {
    alloc_slice docID("dock");
    createRev(db, docID, "1-aaaa"_sl, kFleeceBody);
    _expectedDocumentCount = -1;
    addRevsInParallel(chrono::milliseconds(10), docID, 2, 200);
    runPushReplication(kC4Continuous);
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push From Both Sides", "[Push][Continuous]") {
    alloc_slice docID("doc");
    auto clientOpts = Replicator::Options(kC4Continuous, kC4Continuous)
                        .setProperty(slice(kC4ReplicatorOptionProgressLevel), 1);
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();
    installConflictHandler();

    static const int intervalMs = -500;     // random interval
    static const int iterations = 30;

    atomic_int completed {0};
    unique_ptr<thread> thread1( runInParallel([&]() {
        addRevs(db, chrono::milliseconds(intervalMs), docID, 1, iterations, false);
        if (++completed == 2) {
            sleepFor(chrono::seconds(1)); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }
    }));
    unique_ptr<thread> thread2( runInParallel([&]() {
        addRevs(db2, chrono::milliseconds(intervalMs), docID, 1, iterations, false);
        if (++completed == 2) {
            sleepFor(chrono::seconds(1)); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }
    }));
    
    _expectedDocumentCount = -1;
    _expectedDocPushErrors = {"doc"};
    _checkDocsFinished = false;

    runReplicators(clientOpts, serverOpts);
    thread1->join();
    thread2->join();

    compareDatabases();
}


#pragma mark - ATTACHMENTS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Attachments", "[Push][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
        _expectedDocsFinished.insert("att1");
    }

    auto opts = Replicator::Options::pushing().setProperty(C4STR(kC4ReplicatorOptionProgressLevel), 2);
    runReplicators(opts, Replicator::Options::passive());

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");

    checkAttachments(db2, blobKeys, attachments);
    CHECK(_blobPushProgressCallbacks >= 2);
    CHECK(_blobPullProgressCallbacks == 0);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Attachments", "[Pull][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
        _expectedDocsFinished.insert("att1");
    }

    auto pullOpts = Replicator::Options::pulling().setProperty(C4STR(kC4ReplicatorOptionProgressLevel), 2);
    auto serverOpts = Replicator::Options::passive().setProperty(C4STR(kC4ReplicatorOptionProgressLevel), 2);
    runReplicators(serverOpts, pullOpts);

    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":1}");

    checkAttachments(db2, blobKeys, attachments);
    CHECK(_blobPushProgressCallbacks >= 2);
    CHECK(_blobPullProgressCallbacks >= 2);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Large Attachments", "[Pull][blob]") {
    string att1(100000, '!');
    string att2( 80000, '?');
    string att3(110000, '/');
    string att4(  3000, '.');
    vector<string> attachments = {att1, att2, att3, att4};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
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
        char docid[100], body[100];
        for (int iDoc = 0; iDoc < kNumDocs; ++iDoc) {
            //Log("Creating doc %3d ...", iDoc);
            vector<string> attachments;
            attachments.reserve(1000);
            for (int iAtt = 0; iAtt < kNumBlobsPerDoc; iAtt++) {
                sprintf(body, "doc#%d attachment #%d", iDoc, iAtt);
                attachments.push_back(body);
            }
            sprintf(docid, "doc%03d", iDoc);
            addDocWithAttachments(c4str(docid), attachments, "text/plain");
            _expectedDocsFinished.insert(docid);
            ++_expectedDocumentCount;
        }
    }

    auto pullOpts = Replicator::Options::pulling().setProperty(C4STR(kC4ReplicatorOptionProgressLevel), 2);
    runReplicators(Replicator::Options::passive(), pullOpts);

    compareDatabases();

    validateCheckpoints(db2, db, format("{\"remote\":%d}", kNumDocs).c_str());
    CHECK(_blobPushProgressCallbacks == 0);
    CHECK(_blobPullProgressCallbacks >= kNumDocs*kNumBlobsPerDoc);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Uncompressible Blob", "[Push][blob]") {
    // Test case for issue #354
    alloc_slice image = readFile(sFixturesDir + "for#354.jpg");
    vector<string> attachments = {string((const char*)image.buf, image.size)};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        // Use type text/plain so the replicator will try to compress the attachment
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");

    checkAttachments(db2, blobKeys, attachments);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Blobs Legacy Mode", "[Push][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }

    auto serverOpts = Replicator::Options::passive().setProperty("disable_blob_support"_sl, true);
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);

    checkAttachments(db2, blobKeys, attachments);

    string json = getDocJSON(db2, "att1"_sl);
    replace(json, '"', '\'');
    CHECK(json ==
          "{'_attachments':{'blob_/attached/0':{'content_type':'text/plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27,'revpos':1,'stub':true},"
                           "'blob_/attached/1':{'content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=','length':10,'revpos':1,'stub':true},"
                           "'blob_/attached/2':{'content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=','length':0,'revpos':1,'stub':true}},"
           "'attached':[{'@type':'blob','content_type':'text/plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27},"
                       "{'@type':'blob','content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=','length':10},"
                       "{'@type':'blob','content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=','length':0}]}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Blobs Legacy Mode", "[Push][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain"); //legacy
        _expectedDocumentCount = 1;
    }

    auto serverOpts = Replicator::Options::passive().setProperty("disable_blob_support"_sl, true);
    runReplicators(serverOpts, Replicator::Options::pulling(kC4OneShot));

    checkAttachments(db2, blobKeys, attachments);
}


#pragma mark - FILTERS & VALIDATION:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "DocID Filtered Replication", "[Push][Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");

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
        auto pushOptions = Replicator::Options::pushing();
        pushOptions.properties = properties;
        _expectedDocumentCount = 3;
        runReplicators(pushOptions,
                       Replicator::Options::passive());
    }
    SECTION("Pull") {
        auto pullOptions = Replicator::Options::pulling();
        pullOptions.properties = properties;
        _expectedDocumentCount = 3;
        runReplicators(Replicator::Options::passive(),
                       pullOptions);
    }

    CHECK(c4db_getDocumentCount(db2) == 3);
    c4::ref<C4Document> doc = c4doc_get(db2, "0000001"_sl, true, nullptr);
    CHECK(doc != nullptr);
    doc = c4doc_get(db2, "0000010"_sl, true, nullptr);
    CHECK(doc != nullptr);
    doc = c4doc_get(db2, "0000100"_sl, true, nullptr);
    CHECK(doc != nullptr);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Channels", "[Pull]") {
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey("filter"_sl);
    enc.writeString("Melitta"_sl);
    enc.endDict();
    alloc_slice data = enc.finish();
    auto opts = Replicator::Options::pulling();
    opts.properties = AllocedDict(data);

    // LiteCore's replicator doesn't support filters, so we expect an Unsupported error back:
    _expectedError = {LiteCoreDomain, kC4ErrorUnsupported};
    runReplicators(opts, Replicator::Options::passive());
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Validation Failure", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    auto pullOptions = Replicator::Options::passive();
    atomic<int> validationCount {0};
    pullOptions.callbackContext = &validationCount;
    pullOptions.pullValidator = [](FLString docID, C4RevisionFlags flags, FLDict body, void *context)->bool {
        assert_always(flags == 0);      // can't use CHECK on a bg thread
        ++(*(atomic<int>*)context);
        return (Dict(body)["birthday"].asstring() < "1993");
    };
    _expectedDocPushErrors = set<string>{"0000052", "0000065", "0000071", "0000072"};
    _expectedDocumentCount = 100 - 4;
    runReplicators(Replicator::Options::pushing(),
                   pullOptions);
    validateCheckpoints(db, db2, "{\"local\":100}");
    CHECK(validationCount == 100);
    CHECK(c4db_getDocumentCount(db2) == 96);
}


#pragma mark - CONFLICTS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Conflict", "[Push][Pull][Conflict]") {
    createFleeceRev(db,  C4STR("conflict"), C4STR("1-11111111"), C4STR("{}"));
    _expectedDocumentCount = 1;
    
    // Push db to db2, so both will have the doc:
    runPushReplication();
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(db,  C4STR("conflict"), C4STR("2-2a2a2a2a"), C4STR("{\"db\":1}"));
    createFleeceRev(db2, C4STR("conflict"), C4STR("2-2b2b2b2b"), C4STR("{\"db\":2}"));

    // Verify that rev 1 body is still available, for later use in conflict resolution:
    c4::ref<C4Document> doc = c4doc_get(db, C4STR("conflict"), true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-2a2a2a2a");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-11111111");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"conflict"};
    runReplicators(Replicator::Options::pulling(), Replicator::Options::passive());
    validateCheckpoints(db, db2, "{\"local\":1,\"remote\":2}");

    doc = c4doc_get(db, C4STR("conflict"), true, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
	revID = C4STR("2-2a2a2a2a");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-11111111");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
	revID = C4STR("2-2b2b2b2b");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-11111111");
    CHECK(doc->selectedRev.revID == revID);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Conflict", "[Push][Conflict][NoConflicts]") {
    // In the default no-outgoing-conflicts mode, make sure a local conflict isn't pushed to server:
    auto serverOpts = Replicator::Options::passive();
    createFleeceRev(db,  C4STR("conflict"), C4STR("1-11111111"), C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(db,  C4STR("conflict"), C4STR("2-2a2a2a2a"), C4STR("{\"db\":1}"));
    createFleeceRev(db2, C4STR("conflict"), C4STR("2-2b2b2b2b"), C4STR("{\"db\":2}"));
    REQUIRE(c4db_getLastSequence(db2) == 2);

    // Push db to db2 again:
    _expectedDocumentCount = 0;
    _expectedDocPushErrors = {"conflict"};
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    // Verify db2 didn't change:
    REQUIRE(c4db_getLastSequence(db2) == 2);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Conflict, NoIncomingConflicts", "[Push][Conflict][NoConflicts]") {
    // Put server in no-conflicts mode and verify that a conflict can't be pushed to it.
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();
    createFleeceRev(db,  C4STR("conflict"), C4STR("1-11111111"), C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(db,  C4STR("conflict"), C4STR("2-2a2a2a2a"), C4STR("{\"db\":1}"));
    createFleeceRev(db2, C4STR("conflict"), C4STR("2-2b2b2b2b"), C4STR("{\"db\":2}"));
    REQUIRE(c4db_getLastSequence(db2) == 2);

    // Push db to db2 again:
    _expectedDocumentCount = 0;
    _expectedDocPushErrors = {"conflict"};
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    // Verify db2 didn't change:
    REQUIRE(c4db_getLastSequence(db2) == 2);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Conflict OutgoingConflicts", "[Push][Conflict][NoConflicts]") {
    // Enable outgoing conflicts; verify that a conflict gets pushed to the server.
    auto pushOpts = Replicator::Options::pushing().setProperty(slice(kC4ReplicatorOptionOutgoingConflicts), true);
    auto serverOpts = Replicator::Options::passive();
    createFleeceRev(db,  C4STR("conflict"), C4STR("1-11111111"), C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runReplicators(pushOpts, serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    // Update the doc differently in each db:
    createFleeceRev(db,  C4STR("conflict"), C4STR("2-2a2a2a2a"), C4STR("{\"db\":1}"));
    createFleeceRev(db2, C4STR("conflict"), C4STR("2-2b2b2b2b"), C4STR("{\"db\":2}"));
    REQUIRE(c4db_getLastSequence(db2) == 2);

    // Push db to db2 again:
    runReplicators(pushOpts, serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    // Verify conflict was pushed to db2:
    c4::ref<C4Document> doc = c4doc_get(db2, C4STR("conflict"), true, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
	C4Slice revID = C4STR("2-2b2b2b2b");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(c4doc_selectRevision(doc, C4STR("2-2a2a2a2a"), true, nullptr));
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Then Push No-Conflicts", "[Pull][Push][Conflict][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();

    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody);
    _expectedDocumentCount = 1;

    Log("-------- First Replication db->db2 --------");
    runReplicators(serverOpts,
                   Replicator::Options::pulling());
    validateCheckpoints(db2, db, "{\"remote\":2}");

    Log("-------- Update Doc --------");
    alloc_slice body;
    {
        TransactionHelper t(db2);
        fleece::Encoder enc(c4db_createFleeceEncoder(db2));
        enc.beginDict();
        enc.writeKey("answer"_sl);
        enc.writeInt(666);
        enc.endDict();
        body = enc.finish();
        createRev(db2, kDocID, kRev3ID, body);
        createRev(db2, kDocID, "4-4444"_sl, body);
        _expectedDocumentCount = 1;
    }


    Log("-------- Second Replication db2->db --------");
    runReplicators(serverOpts,
                   Replicator::Options::pushing());
    validateCheckpoints(db2, db, "{\"local\":3,\"remote\":2}");
    compareDatabases();

    Log("-------- Update Doc Again --------");
    createRev(db2, kDocID, "5-5555"_sl, body);
    createRev(db2, kDocID, "6-6666"_sl, body);
    _expectedDocumentCount = 1;

    Log("-------- Third Replication db2->db --------");
    runReplicators(serverOpts,
                   Replicator::Options::pushing());
    validateCheckpoints(db2, db, "{\"local\":5,\"remote\":2}");
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Lost Checkpoint No-Conflicts", "[Push][Conflict][NoConflicts]") {
    auto serverOpts = Replicator::Options::passive().setNoIncomingConflicts();

    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody);

    Log("-------- First Replication: push db->db2 --------");
    _expectedDocumentCount = 1;
    runReplicators(Replicator::Options::pushing(), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");

    clearCheckpoint(db, true);
    Log("-------- Second Replication: push db->db2 --------");
    _expectedDocumentCount = 0;
    runReplicators(Replicator::Options::pushing(), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":2}");
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incoming Deletion Conflict", "[Pull]") {
    C4Slice docID = C4STR("Khan");

    createFleeceRev(db,  docID, C4STR("1-11111111"), C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runPushReplication();

    // Update doc in db, delete it in db2
    createFleeceRev(db,  docID, C4STR("2-88888888"), C4STR("{\"db\":1}"));
    createFleeceRev(db2, docID, C4STR("2-dddddddd"), C4STR("{}"), kRevDeleted);

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"Khan"};
    runReplicators(Replicator::Options::pulling(), Replicator::Options::passive());

    c4::ref<C4Document> doc = c4doc_get(db, docID, true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-88888888");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
	revID = C4STR("2-dddddddd");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    // Resolve the conflict in favor of the remote revision:
    {
        c4::Transaction t(db);
        REQUIRE(t.begin(nullptr));
        C4Error error;
        CHECK(c4doc_resolveConflict(doc, C4STR("2-dddddddd"), C4STR("2-88888888"),
                                    kC4SliceNull, kRevDeleted, &error));
        CHECK(c4doc_save(doc, 0, &error));
        REQUIRE(t.commit(nullptr));
    }
    
    doc = c4doc_get(db, docID, true, nullptr);
	revID = C4STR("2-dddddddd");
    CHECK(doc->revID == revID);

    // Update the doc and push it to db2:
    createRev(db, docID, "3-cafebabe"_sl, kFleeceBody);
    runPushReplication();

    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Local Deletion Conflict", "[Pull][Conflict]") {
    C4Slice docID = C4STR("Khan");

	C4Slice revID =  C4STR("1-11111111");
    createFleeceRev(db,  docID, revID, C4STR("{}"));
    _expectedDocumentCount = 1;

    // Push db to db2, so both will have the doc:
    runPushReplication();

    // Delete doc in db, update it in db2
    createFleeceRev(db,  docID, C4STR("2-dddddddd"), C4STR("{}"), kRevDeleted);
    createFleeceRev(db2, docID, C4STR("2-88888888"), C4STR("{\"db\":1}"));

    // Now pull to db from db2, creating a conflict:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"Khan"};
    runReplicators(Replicator::Options::pulling(), Replicator::Options::passive());

    c4::ref<C4Document> doc = c4doc_get(db, docID, true, nullptr);
    REQUIRE(doc);
	revID = C4STR("2-dddddddd");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
	revID = C4STR("2-88888888");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    // Resolve the conflict in favor of the remote revision:
    {
        c4::Transaction t(db);
        REQUIRE(t.begin(nullptr));
        C4Error error;
        CHECK(c4doc_resolveConflict(doc, C4STR("2-88888888"), C4STR("2-dddddddd"),
                                    kC4SliceNull, kRevDeleted, &error));
        CHECK(c4doc_save(doc, 0, &error));
        REQUIRE(t.commit(nullptr));
    }

    doc = c4doc_get(db, docID, true, nullptr);
    CHECK(doc->revID == revID);

    // Update the doc and push it to db2:
    createRev(db, docID, "3-cafebabe"_sl, kFleeceBody);
    runPushReplication();

    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Server Conflict Branch-Switch", "[Pull][Conflict]") {
    // For https://github.com/couchbase/sync_gateway/issues/3359
    C4Slice docID = C4STR("Khan");

    {
        TransactionHelper t(db);
        createRev(db,  docID, C4STR("1-11111111"), kFleeceBody);
        createConflictingRev(db, docID, C4STR("1-11111111"), C4STR("2-22222222"));
        createConflictingRev(db, docID, C4STR("1-11111111"), C4STR("2-ffffffff"));
        createConflictingRev(db, docID, C4STR("2-22222222"), C4STR("3-33333333"));
    }
    _expectedDocumentCount = 1;
    runPullReplication();

    c4::ref<C4Document> doc = c4doc_get(db2, docID, true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("3-33333333");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->flags & kDocConflicted) == 0);  // locally in db there is no conflict

    {
        TransactionHelper t(db);
        createConflictingRev(db, docID, C4STR("3-33333333"), C4STR("4-dddddddd"), kFleeceBody, kRevDeleted);
    }

    doc = c4doc_get(db, docID, true, nullptr);
    REQUIRE(doc);
	revID = C4STR("2-ffffffff");
    CHECK(doc->revID == revID);
    CHECK(doc->selectedRev.revID == revID);

    SECTION("Unmodified") {
        Log("-------- Second pull --------");
        runPullReplication();

        doc = c4doc_get(db2, docID, true, nullptr);
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->flags & kDocConflicted) == 0);
    }

    SECTION("Modify before 2nd pull") {
        {
            TransactionHelper t(db2);
            createRev(db2, docID, C4STR("4-4444"), kC4SliceNull);
            _expectedDocPullErrors = {"Khan"};
        }

        Log("-------- Second pull --------");
        runPullReplication();

        doc = c4doc_get(db2, docID, true, nullptr);
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
            C4Error error;
            CHECK(c4doc_resolveConflict(doc, C4STR("4-4444"), C4STR("2-ffffffff"), kC4SliceNull, 0, &error));
            CHECK(c4doc_save(doc, 0, &error));
        }

        doc = c4doc_get(db2, docID, true, nullptr);
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


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Doc Notifications", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    for (int i = 1; i <= 100; ++i)
        _expectedDocsFinished.insert(format("%07d", i));
    auto opts = Replicator::Options::pushing();
    opts.setProperty(slice(kC4ReplicatorOptionProgressLevel), 1);
    runReplicators(opts, Replicator::Options::passive());
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Doc Notifications", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    for (int i = 1; i <= 100; ++i)
        _expectedDocsFinished.insert(format("%07d", i));
    auto opts = Replicator::Options::pulling();
    opts.setProperty(slice(kC4ReplicatorOptionProgressLevel), 1);
    runReplicators(Replicator::Options::passive(), opts);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "UnresolvedDocs", "[Push][Pull][Conflict]") {
    createFleeceRev(db, C4STR("conflict"), C4STR("1-11111111"), C4STR("{}"));
    createFleeceRev(db, C4STR("non-conflict"), C4STR("1-22222222"), C4STR("{}"));
    createFleeceRev(db, C4STR("db-deleted"), C4STR("1-33333333"), C4STR("{}"));
    createFleeceRev(db, C4STR("db2-deleted"), C4STR("1-44444444"), C4STR("{}"));
    _expectedDocumentCount = 4;
    
    // Push db to db2, so both will have docs:
    runPushReplication();
    
    // Update the docs differently in each db:
    createFleeceRev(db, C4STR("conflict"), C4STR("2-12121212"), C4STR("{\"db\": 1}"));
    createFleeceRev(db2, C4STR("conflict"), C4STR("2-13131313"), C4STR("{\"db\": 2}"));
    createFleeceRev(db, C4STR("db-deleted"), C4STR("2-32323232"), C4STR("{\"db\":2}"), kRevDeleted);
    createFleeceRev(db2, C4STR("db-deleted"), C4STR("2-31313131"), C4STR("{\"db\": 1}"));
    createFleeceRev(db, C4STR("db2-deleted"), C4STR("2-41414141"), C4STR("{\"db\": 1}"));
    createFleeceRev(db2, C4STR("db2-deleted"), C4STR("2-42424242"), C4STR("{\"db\":2}"), kRevDeleted);
    
    // Now pull to db from db2, creating conflicts:
    C4Log("-------- Pull db <- db2 --------");
    _expectedDocPullErrors = set<string>{"conflict", "db-deleted", "db2-deleted"};
    _expectedDocumentCount = 3;
    runReplicators(Replicator::Options::pulling(), Replicator::Options::passive());
    validateCheckpoints(db, db2, "{\"local\":4,\"remote\":7}");
    
    C4Error err = {};
    std::shared_ptr<DBAccess> acc = make_shared<DBAccess>(db, false);
    C4DocEnumerator* e = acc->unresolvedDocsEnumerator(&err);
    
    // verify only returns the conflicted documents, including the deleted ones.
    vector<C4Slice> docIDs = {"conflict"_sl,   "db-deleted"_sl, "db2-deleted"_sl};
    vector<C4Slice> revIDs = {"2-12121212"_sl, "2-32323232"_sl, "2-41414141"_sl};
    vector<bool> deleteds =  {false,           true,            false};

    for (int count = 0; count < 3; ++count) {
        REQUIRE(c4enum_next(e, &err));
        C4DocumentInfo info;
        c4enum_getDocumentInfo(e, &info);
        CHECK(info.docID == docIDs[count]);
        CHECK(info.revID == revIDs[count]);
        CHECK((info.flags & kDocConflicted) == kDocConflicted);
        bool deleted = ((info.flags & kDocDeleted) != 0);
        CHECK(deleted == deleteds[count]);
    }
    CHECK(!c4enum_next(e, &err));
    c4enum_free(e);
}


#pragma mark - DELTA:


static void mutateDoc(C4Database *db, slice docID, function<void(Dict,Encoder&)> mutator) {
    TransactionHelper t(db);
    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, docID, false, &error);
    REQUIRE(doc);
    Dict props = Value::fromData(doc->selectedRev.body).asDict();

    Encoder enc(c4db_createFleeceEncoder(db));
    mutator(props, enc);
    alloc_slice newBody = enc.finish();

    C4String history = doc->selectedRev.revID;
    C4DocPutRequest rq = {};
    rq.body = newBody;
    rq.docID = docID;
    rq.revFlags = (doc->selectedRev.flags & kRevHasAttachments);
    rq.history = &history;
    rq.historyCount = 1;
    rq.save = true;
    doc = c4doc_put(db, &rq, nullptr, &error);
    CHECK(doc);
}


static void mutateDoc(C4Database *db, slice docID, function<void(MutableDict)> mutator) {
    mutateDoc(db, docID, [&](Dict props, Encoder &enc) {
        MutableDict newProps = props.mutableCopy(kFLDeepCopyImmutables);
        mutator(newProps);
        enc.writeValue(newProps);
    });
}


static void mutationsForDelta(C4Database *db) {
    for (int i = 1; i <= 100; i += 7) {
        char docID[20];
        sprintf(docID, "%07u", i);
        mutateDoc(db, slice(docID), [](MutableDict props) {
            props["birthday"_sl] = "1964-11-28"_sl;
            props["memberSince"_sl].remove();
            props["aNewProperty"_sl] = "!!!!";
        });
    }
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Push+Push", "[Push][Delta]") {
    auto serverOpts = Replicator::Options::passive();

    // Push db --> db2:
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Mutate Docs --------");
    mutationsForDelta(db);

    Log("-------- Second Push --------");
    atomic<int> validationCount {0};
    SECTION("No filter") {
    }
    SECTION("With filter") {
        // Using a pull filter forces deltas to be applied earlier, before rev insertion.
        serverOpts.callbackContext = &validationCount;
        serverOpts.pullValidator = [](FLString docID, C4RevisionFlags flags, FLDict body, void *context)->bool {
            assert_always(flags == 0);      // can't use CHECK on a bg thread
            ++(*(atomic<int>*)context);
            return true;
        };
    }

    _expectedDocumentCount = (100+6)/7;
    auto before = DBAccess::gNumDeltasApplied.load();
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();
    CHECK(DBAccess::gNumDeltasApplied - before == 15);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Bigger Delta Push+Push", "[Push][Delta]") {
    static constexpr int kNumDocs = 100, kNumProps = 1000;
    auto serverOpts = Replicator::Options::passive();

    // Push db --> db2:
    {
        TransactionHelper t(db);
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            string docID = format("doc-%03d", docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for (int p = 0; p < kNumProps; ++p) {
                enc.writeKey(format("field%03d", p));
                enc.writeInt(RandomNumber());
            }
            enc.endDict();
            alloc_slice body = enc.finish();
            createNewRev(db, slice(docID), body);
        }
    }

    _expectedDocumentCount = kNumDocs;
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();

    Log("-------- Mutate Docs --------");
    {
        TransactionHelper t(db);
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            string docID = format("doc-%03d", docNo);
            mutateDoc(db, slice(docID), [](Dict doc, Encoder &enc) {
                enc.beginDict();
                for (Dict::iterator i(doc); i; ++i) {
                    enc.writeKey(i.key());
                    auto value = i.value().asInt();
                    if (RandomNumber() % 4 == 0)
                        value = RandomNumber();
                    enc.writeInt(value);
                }
                enc.endDict();
            });
        }
    }

    Log("-------- Second Push --------");
    _expectedDocumentCount = kNumDocs;
    auto before = DBAccess::gNumDeltasApplied.load();
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();
    CHECK(DBAccess::gNumDeltasApplied - before == kNumDocs);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Push+Pull", "[Push][Pull][Delta]") {
    auto serverOpts = Replicator::Options::passive();

    // Push db --> db2:
    importJSONLines(sFixturesDir + "names_100.json");
    _expectedDocumentCount = 100;
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Mutate Docs In db2 --------");
    mutationsForDelta(db2);

    Log("-------- Pull From db2 --------");
    _expectedDocumentCount = (100+6)/7;
    auto before = DBAccess::gNumDeltasApplied.load();
    runReplicators(Replicator::Options::pulling(kC4OneShot), serverOpts);
    compareDatabases();
    CHECK(DBAccess::gNumDeltasApplied - before == 15);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Attachments Push+Push", "[Push][Delta][blob]") {
    // Simulate SG which requires old-school "_attachments" property:
    auto serverOpts = Replicator::Options::passive().setProperty("disable_blob_support"_sl, true);

    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    Log("-------- Push To db2 --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    Log("-------- Mutate Doc In db --------");
    // Simulate modifying an attachment. In order to avoid having to save a new blob to the db,
    // use the same digest as the 2nd blob.
    mutateDoc(db, "att1"_sl, [](MutableDict rev) {
        auto atts = rev["attached"_sl].asArray().asMutable();
        auto blob = atts[0].asDict().asMutable();
        blob["digest"_sl] = "sha1-rATs731fnP+PJv2Pm/WXWZsCw48=";
        blob["content_type"_sl] = "image/jpeg";
    });

    Log("-------- Push To db2 Again --------");
    _expectedDocumentCount = 1;
    auto before = DBAccess::gNumDeltasApplied.load();
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    CHECK(DBAccess::gNumDeltasApplied - before == 1);

    c4::ref<C4Document> doc2 = c4doc_get(db2, "att1"_sl, true, nullptr);
    alloc_slice json = c4doc_bodyAsJSON(doc2, true, nullptr);
    CHECK(string(json) ==
          "{\"_attachments\":{\"blob_/attached/0\":{\"content_type\":\"image/jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":27,\"revpos\":1,\"stub\":true},"
          "\"blob_/attached/1\":{\"content_type\":\"text/plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":10,\"revpos\":1,\"stub\":true},"
          "\"blob_/attached/2\":{\"content_type\":\"text/plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=\",\"length\":0,\"revpos\":1,\"stub\":true}},"
          "\"attached\":[{\"@type\":\"blob\",\"content_type\":\"image/jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":27},"
          "{\"@type\":\"blob\",\"content_type\":\"text/plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":10},"
          "{\"@type\":\"blob\",\"content_type\":\"text/plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=\",\"length\":0}]}");
}



TEST_CASE_METHOD(ReplicatorLoopbackTest, "Delta Attachments Push+Pull", "[Push][Pull][Delta][blob]") {
    // Simulate SG which requires old-school "_attachments" property:
    auto serverOpts = Replicator::Options::passive().setProperty("disable_blob_support"_sl, true);

    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    Log("-------- Push Doc To db2 --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":1}");

    Log("-------- Mutate Doc In db2 --------");
    // Simulate modifying an attachment. In order to avoid having to save a new blob to the db,
    // use the same digest as the 2nd blob.
    mutateDoc(db2, "att1"_sl, [](MutableDict rev) {
        auto atts = rev["_attachments"_sl].asDict().asMutable();
        auto blob = atts["blob_/attached/0"_sl].asDict().asMutable();
        blob["digest"_sl] = "sha1-rATs731fnP+PJv2Pm/WXWZsCw48=";
        blob["content_type"_sl] = "image/jpeg";
    });

    Log("-------- Pull From db2 --------");
    _expectedDocumentCount = 1;
    auto before = DBAccess::gNumDeltasApplied.load();
    runReplicators(Replicator::Options::pulling(kC4OneShot), serverOpts);
    CHECK(DBAccess::gNumDeltasApplied - before == 1);

    c4::ref<C4Document> doc = c4doc_get(db, "att1"_sl, true, nullptr);
    alloc_slice json = c4doc_bodyAsJSON(doc, true, nullptr);
    CHECK(string(json) ==
          "{\"attached\":[{\"@type\":\"blob\",\"content_type\":\"image/jpeg\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":27},"
          "{\"@type\":\"blob\",\"content_type\":\"text/plain\",\"digest\":\"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=\",\"length\":10},"
          "{\"@type\":\"blob\",\"content_type\":\"text/plain\",\"digest\":\"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=\",\"length\":0}]}");
}
