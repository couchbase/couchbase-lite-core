//
//  ReplicatorCollectionTest.cc
//
//  Copyright 2022-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "ReplicatorLoopbackTest.hh"
#include "c4Collection.hh"
#include "c4Database.hh"

#ifdef ENABLE_REPLICATOR_COLLECTION_TEST

static constexpr slice GuitarsName = "guitars"_sl;
static constexpr C4CollectionSpec Guitars = { GuitarsName, kC4DefaultScopeID };

static constexpr slice RosesName = "roses"_sl;
static constexpr slice TulipsName = "tulips"_sl;
static constexpr slice LavenderName = "lavenders"_sl;
static constexpr slice FlowersScopeName = "flowers"_sl;

static constexpr C4CollectionSpec Roses = { RosesName, FlowersScopeName };
static constexpr C4CollectionSpec Tulips = { TulipsName, FlowersScopeName };
static constexpr C4CollectionSpec Lavenders = { LavenderName, FlowersScopeName };
static constexpr C4CollectionSpec Default = kC4DefaultCollectionSpec;

using namespace std;
using namespace litecore::repl;

using CollectionSpec = C4Database::CollectionSpec;
using CollectionOptions = Options::CollectionOptions;

class ReplicatorCollectionTest : public ReplicatorLoopbackTest {
public:
    ReplicatorCollectionTest() {
        db->createCollection(Guitars);
        db->createCollection(Roses);
        db->createCollection(Tulips);
        db->createCollection(Lavenders);
        
        db2->createCollection(Guitars);
        db2->createCollection(Roses);
        db2->createCollection(Tulips);
        db2->createCollection(Lavenders);
    }
    
    // Push from db1 to db2
    void runPushReplication(vector<CollectionSpec>specs1,
                            vector<CollectionSpec>specs2,
                            C4ReplicatorMode activeMode =kC4OneShot)
    {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, activeMode, kC4Disabled);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2);
    }

    // Pull from db1 to db2
    void runPullReplication(vector<CollectionSpec>specs1,
                            vector<CollectionSpec>specs2,
                            C4ReplicatorMode activeMode =kC4OneShot) {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, kC4Passive, kC4Passive);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Disabled, activeMode);
        runReplicationCollections(coll1, coll2);
    }

    void runPushPullReplication(vector<CollectionSpec>specs1,
                                vector<CollectionSpec>specs2,
                                C4ReplicatorMode activeMode =kC4OneShot) {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, kC4Disabled, activeMode);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2);
    }
    
    void runReplicationCollections(vector<C4ReplicationCollection>& coll1,
                                   vector<C4ReplicationCollection>& coll2)
    {
        C4ReplicatorParameters params1 {};
        params1.collections = coll1.data();
        params1.collectionCount = (unsigned) coll1.size();
        Options opts1 = Options(params1);
        
        C4ReplicatorParameters params2 {};
        params2.collections = coll2.data();
        params2.collectionCount = (unsigned) coll2.size();
        Options opts2 = Options(params2);
        
        runReplicators(opts1, opts2);
    }
    
    vector<CollectionSpec> getCollectionSpecs(C4Database* db, slice scope) {
        vector<CollectionSpec> specs;
        db->forEachCollection(scope, [&](C4CollectionSpec spec) {
            specs.push_back(spec);
        });
        return specs;
    }
    
    C4Collection* getCollection(C4Database* db, CollectionSpec spec, bool mustExist =true) {
        auto coll = db->getCollection(spec);
        Assert(!mustExist || coll != nil);
        return coll;
    }
    
    // Add new docs to a collection
    int addCollDocs(C4Database* db, CollectionSpec spec, int total) {
        C4Collection* coll = getCollection(db, spec);
        string prefix = (db == this->db ? "newdoc-db1-" : (db == this->db2 ? "newdoc-db2-" : "newdoc-"));
        int docNo = 1;
        for (int i = 1; docNo <= total; i++) {
            Log("-------- Creating %d docs --------", i);
            TransactionHelper t(db);
            char docID[20];
            sprintf(docID, "%s%d", prefix.c_str(), docNo++);
            createRev(coll, c4str(docID), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
        }
        Log("-------- Done creating docs --------");
        return docNo - 1;
    }
    
private:
    
    vector<C4ReplicationCollection> replCollections(vector<CollectionSpec>& specs,
                                                    C4ReplicatorMode pushMode,
                                                    C4ReplicatorMode pullMode)
    {
        vector<C4ReplicationCollection> colls(specs.size());
        for (int i = 0; i< specs.size(); i++) {
            colls[i].collection = specs[i];
            colls[i].push = pushMode;
            colls[i].pull = pullMode;
        }
        return colls;
    }
};

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Nonexisting Collections", "[Push][Pull]") {
    vector<CollectionSpec>specs = {CollectionSpec("dummy1"_sl), CollectionSpec("dummy2"_sl)};
    _expectedError = {LiteCoreDomain, kC4ErrorNotFound};
    runPushPullReplication(specs, specs);
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Unmatched Collections", "[Push][Pull]") {
    _expectedError = {LiteCoreDomain, kC4ErrorNotFound};
    runPushPullReplication({Roses, Lavenders}, {Tulips, Lavenders});
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Zero Collections", "[Push][Pull]") {
    _expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
    runPushPullReplication({}, {});
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Default Collection", "[Push][Pull]") {
    addCollDocs(db, Default, 10);
    addCollDocs(db2, Default, 10);
    
    SECTION("PUSH") {
        _expectedDocumentCount = 10;
        runPushReplication({Default}, {Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }
    
    SECTION("PULL") {
        _expectedDocumentCount = 10;
        runPullReplication({Default}, {Default});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
    
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Default}, {Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":40,\"remote\":40}");
    }
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Single Collection", "[Push][Pull]") {
    addCollDocs(db, Guitars, 10);
    addCollDocs(db2, Guitars, 10);
    
    SECTION("PUSH") {
        _expectedDocumentCount = 10;
        runPushReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }
    
    SECTION("PULL") {
        _expectedDocumentCount = 10;
        runPullReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
 
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":20,\"remote\":20}");
    }
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Multiple Collections", "[Push][Pull]") {
    addCollDocs(db, Roses, 10);
    addCollDocs(db, Tulips, 10);
    addCollDocs(db, Lavenders, 10);
    addCollDocs(db2, Roses, 20);
    addCollDocs(db2, Tulips, 20);
    addCollDocs(db2, Lavenders, 20);
    
    SECTION("PUSH") {
        _expectedDocumentCount = 20;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    }
    
    SECTION("PULL") {
        _expectedDocumentCount = 20;
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
        validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    }
    
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 60;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":20}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":20}");
    }
    
    SECTION("PUSH CONTINUOUS") {
        _expectedDocumentCount = 20;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    }
    
    SECTION("PULL CONTINUOUS") {
        _expectedDocumentCount = 20;
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips}, kC4Continuous);
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
        validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    }
    
    SECTION("PUSH and PULL CONTINUOUS") {
        _expectedDocumentCount = 60;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":20}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":20}");
    }
}

#endif
