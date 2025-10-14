//
// ReplicatorVVUpgradeTest.cc
//
// Copyright © 2023 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "ReplicatorLoopbackTest.hh"
#include "c4Collection.h"
#include "Defer.hh"

static alloc_slice makeRealishVector(const char* suffix, uint64_t* unixTs = nullptr) {
    uint64_t ts = 0;
    if ( unixTs && *unixTs != 0 ) {
        ts = *unixTs;
    } else {
        ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        if ( unixTs ) *unixTs = ts;
    }

    const string src = to_string(ts) + suffix;
    return alloc_slice(src);
}

/** For testing scenarios where a database is upgraded to version vectors after it's already
    replicated with a peer.*/
class ReplicatorVVUpgradeTest : public ReplicatorLoopbackTest {
  public:
    ReplicatorVVUpgradeTest() : ReplicatorLoopbackTest(0) {}  // always start in rev-tree mode

    /// Loads names_100.json into db, and bidirectionally syncs with db2.
    void populateAndSync() {
        importJSONLines(sFixturesDir + "names_100.json", _collDB1);

        Log("-------- First Replication (Rev Trees) --------");
        _expectedDocumentCount = 100;
        runPushPullReplication();
        compareDatabases();
        validateCheckpoints(db, db2, "{\"local\":100}");
    }

    /// Reopens both databases, enabling version vectors in both.
    void upgrade(bool fakeClock = false) {
        upgradeToVersionVectors(db, fakeClock);
        _collDB1 = createCollection(db, _collSpec);
        upgradeToVersionVectors(db2, fakeClock);
        _collDB2 = createCollection(db2, _collSpec);
    }
};

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Push After VV Upgrade", "[Push][Upgrade]") {
    //- db pushes docs to db2. Both are still on rev-trees.
    //- db and db2 both upgrade to version vectors.
    //- db updates two of the docs it pushed, and creates a new one.
    //- db pushes to db2 again.

    auto serverOpts = Replicator::Options::passive(_collSpec);

    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    Log("-------- First Replication --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":100}");

    upgrade();
    createNewRev(_collDB1, "0000001"_sl, kFleeceBody);
    createNewRev(_collDB1, "0000002"_sl, kFleeceBody);
    createNewRev(_collDB1, "newDoc"_sl, kFleeceBody);
    _expectedDocumentCount = 3;

    Log("-------- Second Replication (Version Vectors) --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":103}");
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Pull After VV Upgrade", "[Pull][Upgrade]") {
    //- db pushes docs to db2. Both are still on rev-trees.
    //- db and db2 both upgrade to version vectors.
    //- db updates two of the docs it pushed, and creates a new one.
    //- db pushes to db2 again.

    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    Log("-------- First Replication --------");
    runPullReplication();

    upgrade();
    createNewRev(_collDB1, "0000001"_sl, kFleeceBody);
    createNewRev(_collDB1, "0000002"_sl, kFleeceBody);
    createNewRev(_collDB1, "newDoc"_sl, kFleeceBody);
    _expectedDocumentCount = 3;

    Log("-------- Second Replication (Version Vectors) --------");
    runPullReplication();

    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Push and Pull New Docs After VV Upgrade", "[Push][Pull][Upgrade]") {
    populateAndSync();

    Log("-------- Create a doc in each db --------");
    createRev(_collDB1, "new1"_sl, "1-abcd"_sl, kFleeceBody);
    createRev(_collDB2, "new2"_sl, "1-fedc"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    upgrade();

    Log("-------- Second Replication (Version Vectors) --------");
    runPushPullReplication();
    compareDatabases();
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Push and Pull Existing Docs After VV Upgrade", "[Push][Pull][Upgrade]") {
    populateAndSync();

    Log("-------- Update existing doc in each db --------");
    createRev(_collDB1, "0000010"_sl, "2-1111"_sl, kFleeceBody);
    createRev(_collDB2, "0000020"_sl, "2-2222"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    upgrade();

    Log("-------- Second Replication (Version Vectors) --------");
    runPushPullReplication();
    compareDatabases();
    Log("-------- Done --------");
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Resolve Rev-Tree Conflicts After VV Upgrade", "[Conflicts][Upgrade][Pull]") {
    const auto  docName    = "test"_sl;
    const slice kDoc1Rev2A = "2-1111"_sl;
    const slice kDoc1Rev2B = "2-2222"_sl;

    slice left, right, winner, loser, body, resultingRevID;

    const char* sectionName;
    switch ( GENERATE(0, 1, 2, 3, 4) ) {
        case 0:
            sectionName = "Local Lower Wins";
            left = winner = kDoc1Rev2A;
            right = loser  = kDoc1Rev2B;
            body           = kFLSliceNull;
            resultingRevID = "22222000000@Revision+Tree+Encoding"_sl;
            break;
        case 1:  // CBL-7500
            sectionName = "Remote Lower Wins";
            left = winner = kDoc1Rev2B;
            right = loser  = kDoc1Rev2A;
            body           = kFLSliceNull;
            resultingRevID = "22222000000@Revision+Tree+Encoding"_sl;
            break;
        case 2:  // CBL-7500
            sectionName = "Local Higher Wins";
            left = winner = kDoc1Rev2B;
            right = loser  = kDoc1Rev2A;
            body           = kFLSliceNull;
            resultingRevID = "22222000000@Revision+Tree+Encoding"_sl;
            break;
        case 3:
            sectionName = "Remote Higher Wins";
            right = winner = kDoc1Rev2B;
            left = loser   = kDoc1Rev2A;
            body           = kFLSliceNull;
            resultingRevID = "2-2222"_sl;
            break;
        case 4:
            sectionName = "Merge";
            left = winner = kDoc1Rev2A;
            right = loser  = kDoc1Rev2B;
            body           = kFleeceBody;
            resultingRevID = "21111000000@Revision+Tree+Encoding"_sl;
            break;
        default:
            throw logic_error("unreachable");
    }

    DYNAMIC_SECTION("" << sectionName) {
        createFleeceRev(_collDB1, docName, "1-1111"_sl, "{}"_sl);
        createFleeceRev(_collDB1, docName, left, "{\"db\":1}"_sl);
        createFleeceRev(_collDB2, docName, right, "{\"db\":2}"_sl);

        upgrade();
        syncDBConfig();

        _expectedDocPullErrors = set<string>{docName.asString()};
        _expectedDocumentCount = 1;
        runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));

        auto doc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
        DEFER { c4doc_release(doc); };
        REQUIRE(doc);

        CHECK((doc->flags & kDocConflicted) != 0);
        CHECK(doc->selectedRev.revID == left);
        REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
        CHECK(doc->selectedRev.revID == right);
        CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

        {
            TransactionHelper t(db);
            auto conflictResult = c4doc_resolveConflict(doc, winner, loser, body, kRevDeleted, ERROR_INFO());
            REQUIRE(conflictResult);
            CHECK(c4doc_save(doc, 0, ERROR_INFO()));
        }

        auto finalDoc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
        DEFER { c4doc_release(finalDoc); };
        REQUIRE(finalDoc);
        CHECK(finalDoc->selectedRev.revID == resultingRevID);
    }
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Resolve Mixed Conflicts After VV Upgrade", "[Conflicts][Upgrade][Pull]") {
    const auto        docName    = "test"_sl;
    const slice       kDoc1Rev2A = "2-1111"_sl;
    const alloc_slice kDoc1Rev2B = makeRealishVector("@BobBobBobBobBobBobBobA");

    createFleeceRev(_collDB1, docName, "1-1111"_sl, "{}"_sl);

    slice winner, loser, body, resultingRevID;
    SECTION("Local Rev-Tree Wins") {
        winner = kDoc1Rev2A;
        loser  = kDoc1Rev2B;
        body   = kFLSliceNull;
        createFleeceRev(_collDB1, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB2, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
        resultingRevID = "21111000000@Revision+Tree+Encoding"_sl;
    }

    SECTION("Local VV Wins") {
        winner = kDoc1Rev2B;
        loser  = kDoc1Rev2A;
        body   = kFLSliceNull;
        createFleeceRev(_collDB2, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB1, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
        resultingRevID = winner;
    }

    SECTION("Remote Rev-Tree Wins") {
        winner = kDoc1Rev2A;
        loser  = kDoc1Rev2B;
        body   = kFLSliceNull;
        createFleeceRev(_collDB2, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB1, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
        resultingRevID = "21111000000@Revision+Tree+Encoding"_sl;
    }

    SECTION("Remote VV Wins") {
        winner = kDoc1Rev2B;
        loser  = kDoc1Rev2A;
        body   = kFLSliceNull;
        createFleeceRev(_collDB1, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB2, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
        resultingRevID = winner;
    }

    SECTION("Merge Local Wins") {
        winner = kDoc1Rev2A;
        loser  = kDoc1Rev2B;
        body   = kFleeceBody;
        createFleeceRev(_collDB1, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB2, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
    }

    SECTION("Merge Remote Wins") {
        winner = kDoc1Rev2B;
        loser  = kDoc1Rev2A;
        body   = kFleeceBody;
        createFleeceRev(_collDB1, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
        upgrade();
        syncDBConfig();
        createFleeceRev(_collDB2, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
    }

    _expectedDocPullErrors = set<string>{docName.asString()};
    _expectedDocumentCount = 1;
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));

    auto doc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
    DEFER { c4doc_release(doc); };
    REQUIRE(doc);

    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    {
        TransactionHelper t(db);
        auto              conflictResult = c4doc_resolveConflict(doc, winner, loser, body, kRevDeleted, ERROR_INFO());
        REQUIRE(conflictResult);
        CHECK(c4doc_save(doc, 0, ERROR_INFO()));
    }

    auto finalDoc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
    DEFER { c4doc_release(finalDoc); };
    REQUIRE(finalDoc);
    if ( resultingRevID ) {
        CHECK(finalDoc->selectedRev.revID == resultingRevID);
    } else {
        CHECK(slice(finalDoc->selectedRev.revID).findByte('*'));
    }
}

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Resolve Conflicts After VV Upgrade", "[Conflicts][Upgrade][Pull]") {
    const auto docName = "test"_sl;
    upgrade();
    syncDBConfig();

    uint64_t          ts         = 0;
    const alloc_slice kDoc1Rev2A = makeRealishVector("@AliceAliceAliceAliceAA", &ts);
    const alloc_slice kDoc1Rev2B = makeRealishVector("@BobBobBobBobBobBobBobA", &ts);
    slice             winner, loser, body, resultingRevID;
    SECTION("Left Wins") {
        winner         = kDoc1Rev2A;
        loser          = kDoc1Rev2B;
        body           = kFLSliceNull;
        resultingRevID = winner;
    }

    SECTION("Right Wins") {
        winner         = kDoc1Rev2B;
        loser          = kDoc1Rev2A;
        body           = kFLSliceNull;
        resultingRevID = winner;
    }

    SECTION("Merge") {
        winner = kDoc1Rev2A;
        loser  = kDoc1Rev2B;
        body   = kFleeceBody;
    }

    createFleeceRev(_collDB1, docName, "1@*"_sl, "{}"_sl);
    createFleeceRev(_collDB1, docName, kDoc1Rev2A, "{\"db\":1}"_sl);
    createFleeceRev(_collDB2, docName, kDoc1Rev2B, "{\"db\":2}"_sl);
    _expectedDocPullErrors = set<string>{docName.asString()};
    _expectedDocumentCount = 1;
    runReplicators(Replicator::Options::pulling(kC4OneShot, _collSpec), Replicator::Options::passive(_collSpec));

    auto doc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
    DEFER { c4doc_release(doc); };
    REQUIRE(doc);

    REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);

    {
        TransactionHelper t(db);
        auto              conflictResult = c4doc_resolveConflict(doc, winner, loser, body, kRevDeleted, ERROR_INFO());
        REQUIRE(conflictResult);
        CHECK(c4doc_save(doc, 0, ERROR_INFO()));
    }

    auto finalDoc = c4coll_getDoc(_collDB1, docName, true, kDocGetAll, ERROR_INFO());
    DEFER { c4doc_release(finalDoc); };
    REQUIRE(finalDoc);
    if ( resultingRevID ) {
        CHECK(finalDoc->selectedRev.revID == resultingRevID);
    } else {
        CHECK(slice(finalDoc->selectedRev.revID).findByte('*'));
    }
}