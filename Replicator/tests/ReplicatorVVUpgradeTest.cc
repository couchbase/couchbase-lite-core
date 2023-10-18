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
    }
};

/* FIXME: This test is disabled because it exposes a design issue with version vector upgrades
    and P2P replication. I need more time to think about a proper fix.

    The scenario is:
    - Database A pushes docs to peer B. Both are still on rev-trees.
    - A and B both upgrade to version vectors.
    - A updates one of the docs it pushed.
    - A pushes to B again.
    This should succeed, but instead B reports a conflict.

    In database A, when the doc is upgraded by the replicator its version vector looks like
    (`yyyy@AAAA`, xxxx@?), where `AAAA` is database A's UUID and `?` is the generic "pre-existing rev"
    UUID. That's because database A knows the first revision exists elsewhere (it's marked with
    the "remote #1" marker), while the second revision doesn't.

    However, in database B, the doc's version vector is (`xxxx@BBBB`). That's because database B
    doesn't remember that the revision came from elsewhere. When a passive replicator saves
    incoming revisions, it doesn't have a remote-ID to tag them with. That hasn't been an issue,
    until now.

    So the upshot is that the version vector A sends conflicts with the version vector B has.
    B sees that it has a version from AAAA, but is missing the version from BBBB.
    --Jens
 */
TEST_CASE_METHOD(ReplicatorVVUpgradeTest, "Push After VV Upgrade", "[.disabled]") {
    auto serverOpts = Replicator::Options::passive(_collSpec);

    importJSONLines(sFixturesDir + "names_100.json", _collDB1);
    _expectedDocumentCount = 100;
    Log("-------- First Replication --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);
    validateCheckpoints(db, db2, "{\"local\":100}");

    upgrade();
    createNewRev(_collDB1, "0000001"_sl, kFleeceBody);
    createNewRev(_collDB1, "0000002"_sl, kFleeceBody);
    _expectedDocumentCount = 2;

    Log("-------- Second Replication --------");
    runReplicators(Replicator::Options::pushing(kC4OneShot, _collSpec), serverOpts);

    compareDatabases();
    validateCheckpoints(db, db2, "{\"local\":102}");
}
