//
//  ReplicatorLoopbackTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplicatorLoopbackTest.hh"
#include "Worker.hh"
#include "Timer.hh"
#include <chrono>

using namespace litecore::actor;

constexpr duration ReplicatorLoopbackTest::kLatency;

TEST_CASE("Options password logging redaction") {
    string password("SEEKRIT");
    Encoder enc;
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
    Worker::Options opts(kC4OneShot, kC4Disabled, properties);

    auto str = string(opts);
    Log("Options = %s", str.c_str());
    CHECK(str.find(password) == string::npos);
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
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);
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
    createRev("new1"_sl, kRev2ID, kFleeceBody);
    createRev("new2"_sl, kRev3ID, kFleeceBody);
    _expectedDocumentCount = 2;

    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102}", "2-cc");
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
    createRev("new1"_sl, kRev2ID, kFleeceBody);
    createRev("new2"_sl, kRev3ID, kFleeceBody);
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
    Dict root = Value::fromData(doc->selectedRev.body).asDict();
    Value gender = root.get("gender"_sl, c4db_getFLSharedKeys(db2));
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
    fleeceapi::Encoder enc;
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
    createFleeceRev(db, "doc"_sl, kRevID, kBody);
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
    c4::sliceResult remote(c4doc_getRemoteAncestor(doc, 1));
    CHECK(slice(remote) == kRevID);

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

    // Check that doc is not conflicted in db2:
    doc = c4doc_get(db2, "doc"_sl, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "50-0000"_sl);
    CHECK(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));

    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":50}");
}


#pragma mark - CONTINUOUS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Of Tiny DB", "[Push][Continuous]") {
    createRev(db, "doc1"_sl, "1-11"_sl, kFleeceBody);
    createRev(db, "doc2"_sl, "1-aa"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    _stopOnIdle = true;
    auto pushOpt = Replicator::Options::pushing(kC4Continuous);
    runReplicators(pushOpt, Replicator::Options::passive());
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Of Tiny DB", "[Pull][Continuous]") {
    createRev(db, "doc1"_sl, "1-11"_sl, kFleeceBody);
    createRev(db, "doc2"_sl, "1-aa"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    _stopOnIdle = true;
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


#pragma mark - ATTACHMENTS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Attachments", "[Push][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
        _expectedDocumentCount = 1;
    }
    runPushReplication();
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");

    checkAttachments(db2, blobKeys, attachments);
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Attachments", "[Pull][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
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
            ++_expectedDocumentCount;
        }
    }

    runPullReplication();
    compareDatabases();

    validateCheckpoints(db2, db, format("{\"remote\":%d}", kNumDocs).c_str());
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


#pragma mark - FILTERS & VALIDATION:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "DocID Filtered Replication", "[Push][Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");

    Encoder enc;
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
    Encoder enc;
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
    pullOptions.pullValidatorContext = &validationCount;
    pullOptions.pullValidator = [](FLString docID, FLDict body, void *context)->bool {
        ++(*(atomic<int>*)context);
        return (Dict(body)["birthday"].asstring() < "1993");
    };
    _expectedDocPushErrors = set<string>{"0000052", "0000065", "0000071", "0000072"};
    _expectedDocPullErrors = set<string>{"0000052", "0000065", "0000071", "0000072"};
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
    CHECK(doc->selectedRev.revID == C4STR("2-2a2a2a2a"));
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == C4STR("1-11111111"));
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
    CHECK(doc->selectedRev.revID == C4STR("2-2a2a2a2a"));
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == C4STR("1-11111111"));
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
    CHECK(doc->selectedRev.revID == C4STR("2-2b2b2b2b"));
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == C4STR("1-11111111"));
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
    CHECK(doc->selectedRev.revID == C4STR("2-2b2b2b2b"));
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
        Encoder enc(c4db_createFleeceEncoder(db2));
        enc.beginDict();
        enc.writeKey("answer"_sl);
        enc.writeInt(666);
        enc.endDict();
        body = enc.finish();
    }

    createRev(db2, kDocID, kRev3ID, body);
    createRev(db2, kDocID, "4-4444"_sl, body);
    _expectedDocumentCount = 1;

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
    auto docID = C4STR("Khan");

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
    CHECK(doc->selectedRev.revID == C4STR("2-88888888"));
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == C4STR("2-dddddddd"));
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
    CHECK(doc->revID == C4STR("2-dddddddd"));

    // Update the doc and push it to db2:
    createRev(db, docID, "3-cafebabe"_sl, kFleeceBody);
    runPushReplication();

    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Local Deletion Conflict", "[Pull][Conflict]") {
    auto docID = C4STR("Khan");

    createFleeceRev(db,  docID, C4STR("1-11111111"), C4STR("{}"));
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
    CHECK(doc->selectedRev.revID == C4STR("2-dddddddd"));
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK(doc->selectedRev.revID == C4STR("2-88888888"));
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
    CHECK(doc->revID == C4STR("2-88888888"));

    // Update the doc and push it to db2:
    createRev(db, docID, "3-cafebabe"_sl, kFleeceBody);
    runPushReplication();

    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Server Conflict Branch-Switch", "[Pull][Conflict]") {
    // For https://github.com/couchbase/sync_gateway/issues/3359
    auto docID = C4STR("Khan");

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
    CHECK(doc->selectedRev.revID == C4STR("3-33333333"));
    CHECK((doc->flags & kDocConflicted) == 0);  // locally in db there is no conflict

    {
        TransactionHelper t(db);
        createConflictingRev(db, docID, C4STR("3-33333333"), C4STR("4-dddddddd"), kFleeceBody, kRevDeleted);
    }

    doc = c4doc_get(db, docID, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == C4STR("2-ffffffff"));
    CHECK(doc->selectedRev.revID == C4STR("2-ffffffff"));

    SECTION("Unmodified") {
        Log("-------- Second pull --------");
        runPullReplication();

        doc = c4doc_get(db2, docID, true, nullptr);
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == C4STR("2-ffffffff"));
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
        CHECK(doc->selectedRev.revID == C4STR("4-4444"));
        CHECK((doc->selectedRev.flags & kRevIsConflict) == 0);
        CHECK(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
        CHECK(doc->selectedRev.revID == C4STR("2-ffffffff"));
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
        CHECK(doc->selectedRev.revID == C4STR("4-4444"));
        CHECK(!c4doc_selectNextLeafRevision(doc, false, false, nullptr));
        CHECK(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == C4STR("3-33333333"));
        CHECK(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == C4STR("2-22222222"));
        CHECK(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == C4STR("1-11111111"));
        CHECK(!c4doc_selectParentRevision(doc));
    }
}
