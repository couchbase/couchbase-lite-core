//
//  ReplicatorTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include <iostream>
#include "c4Test.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


static const duration kLatency              = chrono::milliseconds(100);
static const duration kCheckpointSaveDelay  = chrono::milliseconds(500);


ostream& operator<< (ostream& o, fleece::slice s);

ostream& operator<< (ostream& o, fleece::slice s) {
    return o << (C4Slice)s;
}


class ReplicatorTest : public C4Test {
public:
    ReplicatorTest()
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

    ~ReplicatorTest() {
        if (parallelThread)
            parallelThread->join();
        C4Error error;
        c4db_delete(db2, &error);
        c4db_free(db2);
    }

    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        opts1.checkpointSaveDelay = opts2.checkpointSaveDelay = kCheckpointSaveDelay;
        C4Database *dbA = db, *dbB = db2;
        if (opts2.push > kC4Passive || opts2.pull > kC4Passive) {
            // always make A the active (client) side
            swap(dbA, dbB);
            swap(opts1, opts2);
        }

        // Client replicator:
        replA = new Replicator(dbA , provider, {"ws", "srv"}, opts1);

        // Server (passive) replicator:
        Address addrB{"ws", "cli"};
        replB = new Replicator(dbB, provider.createWebSocket(addrB), addrB, opts2);

        provider.connect(replA->webSocket(), replB->webSocket());

        Log("Waiting for replication to complete...");
        while (replA->connection() || replB->connection())
            this_thread::sleep_for(chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
        checkpointID = replA->checkpointID();
    }

    void runInParallel(duration delay, function<void(C4Database*)> callback) {
        C4Error error;
        alloc_slice path = c4db_getPath(db);
        C4Database *parallelDB = c4db_open(path, c4db_getConfig(db), &error);
        REQUIRE(parallelDB != nullptr);

        parallelThread.reset(new thread([=]() mutable {
            this_thread::sleep_for(delay);
            callback(parallelDB);
            c4db_free(parallelDB);
        }));
    }

    void addDocsInParallel(duration interval) {
        runInParallel(interval, [=](C4Database *bgdb) {
            int docNo = 1;
            for (int i = 1; i <= 3; i++) {
                if (i > 1)
                    this_thread::sleep_for(interval);
                Log("-------- Creating %d docs --------", 2*i);
                c4::Transaction t(bgdb);
                t.begin(nullptr);
                for (int j = 0; j < 2*i; j++) {
                    char docID[10];
                    sprintf(docID, "newdoc%d", docNo++);
                    createRev(bgdb, c4str(docID), "1-11"_sl, kFleeceBody);
                }
                t.commit(nullptr);
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
    C4Database* db2;
    Retained<Replicator> replA, replB;
    alloc_slice checkpointID;
    unique_ptr<thread> parallelThread;
    future<void> parallelThreadDone;
};


#pragma mark - THE TESTS:


TEST_CASE_METHOD(ReplicatorTest, "Push Empty DB", "[Push]") {
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorTest, "Push Small Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::pushing(),
                   Replicator::Options::passive());
    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":100}");
}

TEST_CASE_METHOD(ReplicatorTest, "Incremental Push", "[Push]") {
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

TEST_CASE_METHOD(ReplicatorTest, "Pull Empty DB", "[Pull]") {
    runReplicators(Replicator::Options::pulling(),
                   Replicator::Options::passive());
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorTest, "Pull Small Non-Empty DB", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling());
    compareDatabases();
    validateCheckpoints(db2, db, "{\"remote\":100}");
}

TEST_CASE_METHOD(ReplicatorTest, "Incremental Pull", "[Pull]") {
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

TEST_CASE_METHOD(ReplicatorTest, "Continuous Push Starting Empty", "[Push]") {
    addDocsInParallel(chrono::seconds(1));
    runReplicators(Replicator::Options::pushing(kC4Continuous),
                   Replicator::Options::passive());
    //FIX: Stop this when bg thread stops adding docs
}

TEST_CASE_METHOD(ReplicatorTest, "Continuous Pull Starting Empty", "[Pull]") {
    addDocsInParallel(chrono::seconds(1));
    runReplicators(Replicator::Options::passive(),
                   Replicator::Options::pulling(kC4Continuous));
    //FIX: Stop this when bg thread stops adding docs
}
