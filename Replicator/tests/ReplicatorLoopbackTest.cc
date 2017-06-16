//
//  ReplicatorLoopbackTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <thread>

#include "c4Test.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


static const duration kLatency              = chrono::milliseconds(50);
static const duration kCheckpointSaveDelay  = chrono::milliseconds(500);


class ReplicatorLoopbackTest : public C4Test, Replicator::Delegate {
public:
    ReplicatorLoopbackTest()
    :C4Test(0)
    ,provider(kLatency)
    {
        auto db2Path = TempDir() + "cbl_core_test2";
        auto db2PathSlice = c4str(db2Path.c_str());

        auto config = c4db_getConfig(db);
        C4Error error;
        if (!c4db_deleteAtPath(db2PathSlice, config, &error))
            REQUIRE(error.code == 0);
        db2 = c4db_open(db2PathSlice, config, &error);
        REQUIRE(db2 != nullptr);
    }

    ~ReplicatorLoopbackTest() {
        if (parallelThread)
            parallelThread->join();
        replClient = replServer = nullptr;
        C4Error error;
        c4db_delete(db2, &error);
        c4db_free(db2);
    }

    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        _gotResponse = false;
        statusChangedCalls = 0;
        statusReceived = {};

        C4Database *dbServer = db, *dbClient = db2;
        if (opts2.push > kC4Passive || opts2.pull > kC4Passive) {
            // always make opts1 the active (client) side
            swap(dbServer, dbClient);
            swap(opts1, opts2);
        }

        // Client replicator:
        replClient = new Replicator(dbServer, provider, {"ws", "srv"}, *this, opts1);

        // Server (passive) replicator:
        Address addrB{"ws", "cli"};
        replServer = new Replicator(dbClient, provider.createWebSocket(addrB), *this, opts2);

        // Response headers:
        Encoder enc;
        enc.beginDict();
        enc.writeKey("Set-Cookie"_sl);
        enc.writeString("flavor=chocolate-chip");
        enc.endDict();
        AllocedDict headers(enc.finish());

        // Bind the replicators' WebSockets and start them:
        provider.bind(replClient->webSocket(), replServer->webSocket(), headers);
        replClient->start();
        replServer->start();

        Log("Waiting for replication to complete...");
        while (replClient->status().level > kC4Stopped || replServer->status().level > kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
        checkpointID = replClient->checkpointID();
        CHECK(_gotResponse);
        CHECK(statusChangedCalls > 0);
        CHECK(statusReceived.level == kC4Stopped);
        CHECK(statusReceived.progress.completed == statusReceived.progress.total);
        CHECK(statusReceived.error.code == expectedError.code);
        if (expectedError.code)
            CHECK(statusReceived.error.domain == expectedError.domain);
    }

    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const AllocedDict &headers) override {
        if (repl == replClient) {
            CHECK(!_gotResponse);
            _gotResponse = true;
            CHECK(status == 200);
            CHECK(headers["Set-Cookie"].asString() == "flavor=chocolate-chip"_sl);
        }
    }

    virtual void replicatorStatusChanged(Replicator* repl,
                                         const Replicator::Status &status) override
    {
        // Note: Can't use Catch on a background thread
        if (repl == replClient) {
            Assert(_gotResponse);
            ++statusChangedCalls;
            Log(">> Replicator is %-s, progress %lu/%lu",
                kC4ReplicatorActivityLevelNames[status.level],
                (unsigned long)status.progress.completed, (unsigned long)status.progress.total);
            Assert(status.progress.completed <= status.progress.total);
            if (status.progress.total > 0) {
                Assert(status.progress.completed >= statusReceived.progress.completed);
                Assert(status.progress.total     >= statusReceived.progress.total);
            }
            statusReceived = status;
        }
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        if (repl == replClient) {
            Log(">> Replicator closed with code=%d/%d, message=%.*s",
                status.reason, status.code, SPLAT(status.message));
        }
    }


    void runInParallel(function<void(C4Database*)> callback) {
        C4Error error;
        C4Database *parallelDB = c4db_openAgain(db, &error);
        REQUIRE(parallelDB != nullptr);

        parallelThread.reset(new thread([=]() mutable {
            callback(parallelDB);
            c4db_free(parallelDB);
        }));
    }

    void addDocsInParallel(duration interval, int total) {
        runInParallel([=](C4Database *bgdb) {
            int docNo = 1;
            for (int i = 1; docNo <= total; i++) {
                this_thread::sleep_for(interval);
                Log("-------- Creating %d docs --------", 2*i);
                c4::Transaction t(bgdb);
                C4Error err;
                Assert(t.begin(&err));
                for (int j = 0; j < 2*i; j++) {
                    char docID[20];
                    sprintf(docID, "newdoc%d", docNo++);
                    createRev(bgdb, c4str(docID), "1-11"_sl, kFleeceBody);
                }
                Assert(t.commit(&err));
            }
        });
    }

    void compareDatabases() {
        C4Error error;
        c4::ref<C4DocEnumerator> e1 = c4db_enumerateAllDocs(db, nullslice, nullslice, nullptr,  &error);
        REQUIRE(e1);
        c4::ref<C4DocEnumerator> e2 = c4db_enumerateAllDocs(db2, nullslice, nullslice, nullptr,  &error);
        REQUIRE(e2);

        unsigned i = 0;
        while (c4enum_next(e1, &error)) {
            C4DocumentInfo doc1, doc2;
            c4enum_getDocumentInfo(e1, &doc1);
            INFO("db document #" << i << ": '" << asstring(doc1.docID) << "'");
            REQUIRE(c4enum_next(e2, &error));
            c4enum_getDocumentInfo(e2, &doc2);
            REQUIRE(doc1.docID == doc2.docID);
            REQUIRE(doc1.revID == doc2.revID);
            REQUIRE(doc1.flags == doc2.flags);
            ++i;
        }
        REQUIRE(error.code == 0);
    }

    void validateCheckpoint(C4Database *database, bool local,
                            const char *body, const char *meta = "1-") {
        C4Error err;
        c4::ref<C4RawDocument> doc( c4raw_get(database,
                                              (local ? C4STR("checkpoints") : C4STR("peerCheckpoints")),
                                              alloc_slice(checkpointID),
                                              &err) );
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << asstring(checkpointID) << "'; err = " << err.domain << "," << err.code);
        REQUIRE(doc);
        CHECK(doc->body == c4str(body));
        if (!local)
            CHECK(c4rev_getGeneration(doc->meta) >= c4rev_getGeneration(c4str(meta)));
    }

    void validateCheckpoints(C4Database *localDB, C4Database *remoteDB,
                             const char *body, const char *meta = "1-cc") {
        validateCheckpoint(localDB,  true,  body, meta);
        validateCheckpoint(remoteDB, false, body, meta);
    }

    LoopbackProvider provider;
    C4Database* db2 {nullptr};
    Retained<Replicator> replClient, replServer;
    alloc_slice checkpointID;
    unique_ptr<thread> parallelThread;
    bool _gotResponse {false};
    Replicator::Status statusReceived { };
    unsigned statusChangedCalls {0};
    C4Error expectedError {};
};


#pragma mark - THE TESTS:


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty DB", "[Push]") {
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Small Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Empty Docs", "[Push]") {
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":1}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    Log("-------- Second Replication --------");
    createRev("new1"_sl, kRev2ID, kFleeceBody);
    createRev("new2"_sl, kRev3ID, kFleeceBody);

    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102}", "2-cc");

}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Empty DB", "[Pull]") {
    runReplicators(Replicator::Options::pulling(),
                   Replicator::Options::passive());
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Pull Small Non-Empty DB", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Incremental Pull", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");

    Log("-------- Second Replication --------");
    createRev("new1"_sl, kRev2ID, kFleeceBody);
    createRev("new2"_sl, kRev3ID, kFleeceBody);

    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":102}", "2-cc");
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Push Starting Empty", "[Push][.neverending]") {
    addDocsInParallel(chrono::milliseconds(1500), 6);
    runReplicators(Replicator::Options::pushing(kC4Continuous),
                   Replicator::Options::passive());
    //FIX: Stop this when bg thread stops adding docs
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Pull Starting Empty", "[Pull][.neverending]") {
    addDocsInParallel(chrono::milliseconds(1500), 6);
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling(kC4Continuous));
    //FIX: Stop this when bg thread stops adding docs
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Continuous Fast Push", "[Push][.neverending]") {
    addDocsInParallel(chrono::milliseconds(250), 1000000);
    runReplicators(Replicator::Options::pushing(kC4Continuous),
                   Replicator::Options::passive());
    //FIX: Stop this when bg thread stops adding docs
}

TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push Attachments", "[Push][blob]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
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
    }
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
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
    }
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":1}");

    checkAttachments(db2, blobKeys, attachments);
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
    expectedError = {LiteCoreDomain, kC4ErrorUnsupported};
    runReplicators(opts, Replicator::Options::passive());
}


TEST_CASE_METHOD(ReplicatorLoopbackTest, "Push/Pull Active Only", "[Pull]") {
    // Add 100 docs, then delete 50 of them:
    importJSONLines(sFixturesDir + "names_100.json");
    for (unsigned i = 1; i <= 100; i += 2) {
        char docID[20];
        sprintf(docID, "%07u", i);
        createRev(slice(docID), kRev2ID, nullslice, kRevDeleted); // delete it
    }

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
    compareDatabases();

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

    // Push db into db2:
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");

    // Get one of the pushed docs from db2 and look up "gender":
    c4::ref<C4Document> doc = c4doc_get(db2, "0000001"_sl, true, nullptr);
    REQUIRE(doc);
    Dict root = Value::fromData(doc->selectedRev.body).asDict();
    Value gender = root.get("gender"_sl, c4db_getFLSharedKeys(db2));
    REQUIRE(gender != nullptr);
    REQUIRE(gender.asstring() == "female");
}

