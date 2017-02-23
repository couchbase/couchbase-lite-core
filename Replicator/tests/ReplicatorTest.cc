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
#include <chrono>

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
        repl1 = new Replicator(db , provider, {"ws", "one"}, opts1);
        repl2 = new Replicator(db2, provider, {"ws", "two"}, opts2);

        provider.connect(repl1->webSocket(), repl2->webSocket());

        Log("Waiting for replication to complete...");
        while (repl1->connection() || repl2->connection())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
    }

    LoopbackProvider provider;
    C4Database* db2;
    Retained<Replicator> repl1, repl2;

private:
};



TEST_CASE_METHOD(ReplicatorTest, "Push Empty DB") {
    runReplicators({true,  false, false},
                   {false, false, false});
}


TEST_CASE_METHOD(ReplicatorTest, "Push Small Non-Empty DB") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators({true,  false, false},
                   {false, false, false});
}

#if 0
TEST_CASE_METHOD(ReplicatorTest, "Pull Small Non-Empty DB") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicators({false, false, false},
                   {false, true, false});
}
#endif
