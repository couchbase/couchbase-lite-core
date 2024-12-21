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

/** For testing scenarios where a database is upgraded to version vectors after it's already
    replicated with a peer.*/
class ReplicatorVVUpgradeTest : public ReplicatorLoopbackTest {
  public:
    ReplicatorVVUpgradeTest() : ReplicatorLoopbackTest(0) {}

    // Reopen database, enabling version vectors:
    void upgrade(C4Database*& database, C4Collection*& coll1) {
        alloc_slice name(c4db_getName(database));
        REQUIRE(c4db_close(database, WITH_ERROR()));
        c4db_release(database);
        database = nullptr;
        coll1    = nullptr;

        C4Log("---- Reopening '%.*s' with version vectors ---", FMTSLICE(name));
        C4DatabaseConfig2 config = dbConfig();
        config.flags |= kC4DB_VersionVectors;
        database = c4db_openNamed(name, &config, ERROR_INFO());
        REQUIRE(database);
        coll1 = createCollection(database, _collSpec);
    }

    // Reopen both databases, enabling version vectors in both.
    void upgrade() {
        upgrade(db, _collDB1);
        upgrade(db2, _collDB2);
        syncDBConfig();
    }
};

#if 0
// Disabled, c.f. https://jira.issues.couchbase.com/browse/CBL-6593
TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Push After VV Upgrade", "[Push]") {
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

    Log("-------- Second Replication --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":103}");
}
#endif

TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Pull After VV Upgrade", "[Pull]") {
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

    Log("-------- Second Replication --------");
    runPullReplication();

    compareDatabases();
}
