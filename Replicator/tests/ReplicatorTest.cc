//
//  ReplicatorTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "c4.hh"
#include <algorithm>
#include <chrono>

using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


class ReplicatorTest : public C4Test {
public:
    ReplicatorTest()
    :C4Test(0)
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
        C4Error error;
        c4db_delete(db2, &error);
        c4db_free(db2);
    }

    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        C4Database *dbA = db, *dbB = db2;
        if (opts2.push || opts2.pull) {         // always make A the active (client) side
            std::swap(dbA, dbB);
            std::swap(opts1, opts2);
        }

        // Client replicator:
        replA = new Replicator(dbA , provider, {"ws", "srv"}, opts1);

        // Server (passive) replicator:
        Address addrB{"ws", "cli"};
        replB = new Replicator(dbB, provider.createWebSocket(addrB), addrB);

        provider.connect(replA->webSocket(), replB->webSocket());

        Log("Waiting for replication to complete...");
        while (replA->connection() || replB->connection())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
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

    LoopbackProvider provider;
    C4Database* db2;
    Retained<Replicator> replA, replB;

private:
};



TEST_CASE_METHOD(ReplicatorTest, "Push Empty DB", "[Push]") {
    runReplicators({true,  false, false},
                   {false, false, false});
    compareDatabases();
}


TEST_CASE_METHOD(ReplicatorTest, "Push Small Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators({true,  false, false},
                   {false, false, false});
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorTest, "Pull Empty DB", "[Pull]") {
    runReplicators({false, true,  false},
                   {false, false, false});
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorTest, "Pull Small Non-Empty DB", "[Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators({false, false,  false},
                   {false, true, false});
    compareDatabases();
}
