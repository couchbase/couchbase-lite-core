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

    C4Database* db2;

private:
};



TEST_CASE_METHOD(ReplicatorTest, "Replicate") {
    LoopbackProvider provider;
    Address addr1("ws", "one"), addr2("ws", "two");
    Retained<Replicator> repl1 = new Replicator(db , provider, addr2, {true, false, false});
    Retained<Replicator> repl2 = new Replicator(db2, provider, addr1, {false, false, false});

    provider.connect(repl1->webSocket(), repl2->webSocket());

    Log("Waiting for replication to complete...");
    std::this_thread::sleep_for(std::chrono::seconds(1000));
}
