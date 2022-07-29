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
                                C4ReplicatorMode activeMode =kC4OneShot,
                                bool reset =false)
    {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, kC4Disabled, activeMode);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2, reset);
    }
    
    void runReplicationCollections(vector<C4ReplicationCollection>& coll1,
                                   vector<C4ReplicationCollection>& coll2,
                                   bool reset =false)
    {
        C4ReplicatorParameters params1 {};
        params1.collectionCount = coll1.size();
        if (coll1.size() > 0) {
            params1.collections = coll1.data();
        }
        Options opts1 = Options(params1);
        
        C4ReplicatorParameters params2 {};
        params2.collectionCount = coll2.size();
        if (coll2.size() > 0) {
            params2.collections = coll2.data();
        }
        Options opts2 = Options(params2);

        runReplicators(opts1, opts2, reset);
    }
    
    vector<CollectionSpec> getCollectionSpecs(C4Database* db, slice scope) {
        vector<CollectionSpec> specs;
        db->forEachCollection(scope, [&](C4CollectionSpec spec) {
            specs.push_back(spec);
        });
        return specs;
    }
    
    static C4Collection* getCollection(C4Database* db, CollectionSpec spec, bool mustExist =true) {
        auto coll = db->getCollection(spec);
        Assert(!mustExist || coll != nil);
        return coll;
    }
    
    // Add new docs to a collection
    int addCollDocs(C4Database* db, CollectionSpec spec, int total, string prefix = "") {
        C4Collection* coll = getCollection(db, spec);
        if (prefix.empty()) {
            prefix= (db == this->db ? "newdoc-db1-" : (db == this->db2 ? "newdoc-db2-" : "newdoc-"));
        }
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
    
    void purgeAllDocs(C4Database* db, CollectionSpec spec) {
        C4Collection* coll = getCollection(db, spec);
        
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        options.flags &= ~kC4IncludeBodies;
        
        C4Error error;
        auto e = c4coll_enumerateAllDocs(coll, &options, ERROR_INFO(error));
        REQUIRE(e);
        {
            TransactionHelper t(db2);
            while(c4enum_next(e, &error)) {
                C4DocumentInfo info;
                REQUIRE(c4enum_getDocumentInfo(e, &info));
                REQUIRE(c4coll_purgeDoc(coll, info.docID, nullptr));
            }
        }
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
    _expectedError = {WebSocketDomain, 404};
    runPushPullReplication({Roses, Lavenders}, {Tulips, Lavenders});
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Zero Collections", "[Push][Pull]") {
    _expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
    runPushPullReplication({}, {});
}


#define DISABLE_PUSH_AND_PULL
#define DISABLE_CONTINUOUS

static std::set<string> getDocInfos(C4Database* db, C4CollectionSpec coll) {
    std::set<string> ret;
    C4Collection* collection = ReplicatorCollectionTest::getCollection(db, coll);
    auto e = c4coll_enumerateAllDocs(collection, nullptr, ERROR_INFO());
    {
        TransactionHelper t(db);
        while (c4enum_next(e, ERROR_INFO())) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            alloc_slice docID(info.docID);
            alloc_slice revID(info.revID);
            string entry = docID.asString()+"/"+revID.asString();
            ret.insert(entry);
        }
    }
    c4enum_free(e);
    return ret;
}


struct CheckDBEntries {
    CheckDBEntries(C4Database* db, C4Database* db2, vector<C4CollectionSpec> specs)
    : _db(db)
    , _db2(db2)
    {
        for (auto i = 0; i < specs.size(); ++i) {
            _collSpecs.push_back(specs[i]);
            _dbBefore.push_back(getDocInfos(_db, specs[i]));
            _db2Before.push_back(getDocInfos(_db2, specs[i]));
        }
    }
    
    ~CheckDBEntries() {
        vector<set<string>> dbAfter;
        vector<set<string>> db2After;
        for (auto i = 0; i < _collSpecs.size(); ++i) {
            dbAfter.push_back(getDocInfos(_db, _collSpecs[i]));
            db2After.push_back(getDocInfos(_db2, _collSpecs[i]));
            CHECK(dbAfter[i].size() == _dbBefore[i].size());
            for (auto& doc : _dbBefore[i]) {
                CHECK(db2After[i].erase(doc) == 1);
            }
            for (auto& doc : _db2Before[i]) {
                CHECK(db2After[i].erase(doc) == 1);
            }
            REQUIRE(db2After[i].size() == 0);
        }
    }
    
    C4Database* _db;
    C4Database* _db2;
    vector<C4CollectionSpec> _collSpecs;
    vector<set<string>> _dbBefore;
    vector<set<string>> _db2Before;
};


TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Default Collection", "[Push][Pull]") {
    addCollDocs(db, Default, 10);
    addCollDocs(db2, Default, 10);
    
    SECTION("PUSH") {
        CheckDBEntries check(db, db2, {Default});

        _expectedDocumentCount = 10;
        runPushReplication({Default}, {Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }

    SECTION("PULL") {
        CheckDBEntries check(db, db2, {Default});

        _expectedDocumentCount = 10;
        runPullReplication({Default}, {Default});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Default}, {Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":40,\"remote\":40}");
    }
#endif
    SECTION("PUSH with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Default});

        _expectedDocumentCount = 10;
        runPushReplication({Default}, {Guitars, Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }
    
    SECTION("PULL with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Default});

        _expectedDocumentCount = 10;
        runPullReplication({Guitars, Default}, {Default});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL with MULTIPLE PASSIVE COLLECTIONS") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Default}, {Guitars, Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":40,\"remote\":40}");
    }
#endif
}


TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Single Collection", "[Push][Pull]") {
    addCollDocs(db, Guitars, 10);
    addCollDocs(db2, Guitars, 10);

    SECTION("PUSH") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPushReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }

    SECTION("PULL") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPullReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Guitars}, {Guitars, Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":20,\"remote\":20}");
    }
#endif
    SECTION("PUSH with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPushReplication({Guitars}, {Default, Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }

    SECTION("PULL with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPullReplication({Guitars, Default}, {Guitars});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL with MULTIPLE PASSIVE COLLECTIONS") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Guitars}, {Guitars, Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":20,\"remote\":20}");
    }
#endif
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Multiple Collections", "[Push][Pull]") {
    addCollDocs(db, Roses, 10);
    addCollDocs(db, Tulips, 10);
    addCollDocs(db, Lavenders, 10);
    addCollDocs(db2, Roses, 20);
    addCollDocs(db2, Tulips, 20);
    addCollDocs(db2, Lavenders, 20);
    
    SECTION("PUSH") {
        CheckDBEntries check(db, db2, {Roses, Tulips});

        _expectedDocumentCount = 20;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    }

    SECTION("PULL") {
        CheckDBEntries check(db, db2, {Roses, Tulips});

        _expectedDocumentCount = 20;
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
        validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    }

#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 60;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":20}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":20}");
    }
#endif
#ifndef DISABLE_CONTINUOUS
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
#endif
#ifndef DISABLE_PUSH_AND_PULL
    SECTION("PUSH and PULL CONTINUOUS") {
        _expectedDocumentCount = 60;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":20}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":20}");
    }
#endif
}

#ifdef ENABLE_REPLICATOR_COLLECTION_TEST

TEST_CASE_METHOD(ReplicatorCollectionTest, "Incremental Push and Pull", "[Push][Pull]") {
    addCollDocs(db, Roses, 10);
    addCollDocs(db, Tulips, 10);
    addCollDocs(db2, Roses, 10);
    addCollDocs(db2, Tulips, 10);
    
    _expectedDocumentCount = 40;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":10}");
    
    addCollDocs(db, Roses, 1, "rose1");
    addCollDocs(db, Tulips, 2, "tulip1");
    
    addCollDocs(db2, Roses, 3, "rose2");
    addCollDocs(db2, Tulips, 4, "tulip2");
    
    _expectedDocumentCount = 10;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":11,\"remote\":13}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":12,\"remote\":14}");
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Reset Checkpoint with Push and Pull", "[Push][Pull]") {
    addCollDocs(db, Roses, 10);
    addCollDocs(db, Tulips, 10);
    addCollDocs(db2, Roses, 10);
    addCollDocs(db2, Tulips, 10);
    
    _expectedDocumentCount = 40;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":10}");
    
    purgeAllDocs(db, Roses);
    purgeAllDocs(db, Tulips);
    purgeAllDocs(db2, Roses);
    purgeAllDocs(db2, Tulips);
    
    _expectedDocumentCount = 0;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":10}");
    
    _expectedDocumentCount = 40;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4OneShot, true);
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":10}");
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Push and Pull Attachments", "[Push][Pull]") {
    vector<string> attachments1 = {"Attachment A", "Attachment B", ""};
    vector<C4BlobKey> blobKeys1a, blobKeys1b;
    {
        TransactionHelper t(db);
        blobKeys1a = addDocWithAttachments(db, Roses, "doc1"_sl, attachments1, "text/plain");
        blobKeys1b = addDocWithAttachments(db, Tulips, "doc2"_sl, attachments1, "text/plain");
    }
    
    vector<string> attachments2 = {"Attachment C", "Attachment D", ""};
    vector<C4BlobKey> blobKeys2a, blobKeys2b;
    {
        TransactionHelper t(db2);
        blobKeys2a = addDocWithAttachments(db2, Tulips, "doc3"_sl, attachments2, "text/plain");
        blobKeys2b = addDocWithAttachments(db2, Tulips, "doc4"_sl, attachments2, "text/plain");
    }
    
    _expectedDocumentCount = 4;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":2}");
    validateCollectionCheckpoints(db, db2, 1, "{\"remote\":2}");
    
    checkAttachments(db, blobKeys1a, attachments1);
    checkAttachments(db, blobKeys1b, attachments1);
    checkAttachments(db, blobKeys2a, attachments2);
    checkAttachments(db, blobKeys2b, attachments2);
    
    checkAttachments(db2, blobKeys1a, attachments1);
    checkAttachments(db2, blobKeys1b, attachments1);
    checkAttachments(db2, blobKeys2a, attachments2);
    checkAttachments(db2, blobKeys2b, attachments2);
}

#endif
