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
#include "Base64.hh"
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Replicator.h"
#include "fleece/Mutable.hh"

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
    
    using C4Test::addDocs;
    
    // Push from db1 to db2
    void runPushReplication(vector<CollectionSpec>specs1,
                            vector<CollectionSpec>specs2,
                            C4ReplicatorMode activeMode =kC4OneShot,
                            bool reset =false)
    {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, activeMode, kC4Disabled);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2, reset);
    }

    // Pull from db1 to db2
    void runPullReplication(vector<CollectionSpec>specs1,
                            vector<CollectionSpec>specs2,
                            C4ReplicatorMode activeMode =kC4OneShot,
                            bool reset =false)
    {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, kC4Passive, kC4Passive);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Disabled, activeMode);
        runReplicationCollections(coll1, coll2, reset);
    }

    void runPushPullReplication(vector<CollectionSpec>specs1,
                                vector<CollectionSpec>specs2,
                                C4ReplicatorMode activeMode =kC4OneShot,
                                bool reset =false)
    {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, activeMode, activeMode);
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
    
    Options replicatorOptions(vector<CollectionSpec>specs, C4ReplicatorMode pushMode, C4ReplicatorMode pullMode) {
        vector<C4ReplicationCollection> coll = replCollections(specs, pushMode, pullMode);
        C4ReplicatorParameters params {};
        params.collectionCount = coll.size();
        if (coll.size() > 0) {
            params.collections = coll.data();
        }
        return Options(params);
    }
    
    vector<CollectionSpec> getCollectionSpecs(C4Database* db, slice scope) {
        vector<CollectionSpec> specs;
        db->forEachCollection(scope, [&](C4CollectionSpec spec) {
            specs.push_back(spec);
        });
        return specs;
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
        c4enum_free(e);
        CHECK(coll->getDocumentCount() == 0);
    }
    
    class ResolvedDocument {
    public:
        ResolvedDocument()                      =default;   // Resolved as a deleted doc
        ResolvedDocument(C4Document* doc)       :_doc(c4doc_retain(doc)) { }
        ResolvedDocument(FLDict mergedProps)    :_mergedProps(mergedProps) { }
        
        C4Document* doc()                       {return _doc;}
        FLDict mergedProps()                    {return _mergedProps;}
    private:
        c4::ref<C4Document> _doc;
        RetainedDict _mergedProps;
    };
    
    void setConflictResolver(C4Database* activeDB,
                             std::function<ResolvedDocument(CollectionSpec collection,
                                                            C4Document* local,
                                                            C4Document* remote)> resolver)
    {
        REQUIRE(activeDB);
        
        if (!resolver) {
            _conflictHandler = nullptr;
            return;
        }
        
        auto& conflictHandlerRunning = _conflictHandlerRunning;
        _conflictHandler = [activeDB, resolver, &conflictHandlerRunning](ReplicatedRev *rev) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            auto collPath = Options::collectionSpecToPath(rev->collectionSpec);
            Log("Resolving conflict for '%.*s' in '%.*s' ...", SPLAT(rev->docID), SPLAT(collPath));
            
            C4Error error;
            C4Collection* coll = c4db_getCollection(activeDB, rev->collectionSpec, &error);
            Assert(coll, "conflictHandler: Couldn't find collection '%.*s'", SPLAT(collPath));

            conflictHandlerRunning = true;
            TransactionHelper t(activeDB);
            
            // Get the local doc:
            c4::ref<C4Document> localDoc = c4coll_getDoc(coll, rev->docID, true, kDocGetAll, &error);
            Assert(localDoc, "conflictHandler: Couldn't read doc '%.*s' in '%.*s'",
                   SPLAT(rev->docID), SPLAT(collPath));
            
            // Get the remote doc:
            c4::ref<C4Document> remoteDoc = c4coll_getDoc(coll, rev->docID, true, kDocGetAll, &error);
            if (!c4doc_selectNextLeafRevision(remoteDoc, true, false, &error)) {
                Assert(false, "conflictHandler: Couldn't get conflicting remote revision of '%.*s' in '%.*s'",
                       SPLAT(rev->docID), SPLAT(collPath));
            }
            
            ResolvedDocument resolvedDoc;
            if ((localDoc->selectedRev.flags & kRevDeleted) &&
                (remoteDoc->selectedRev.flags & kRevDeleted))
            {
                resolvedDoc = ResolvedDocument(remoteDoc);
            } else {
                resolvedDoc = resolver(coll->getSpec(), localDoc.get(), remoteDoc.get());
            }
            
            FLDict mergedBody = nullptr;
            C4RevisionFlags mergedFlags = 0;
            
            if (resolvedDoc.doc() == remoteDoc) {
                mergedFlags |= resolvedDoc.doc()->selectedRev.flags;
            } else {
                C4Document* resDoc = resolvedDoc.doc();
                FLDict mergedProps = resolvedDoc.mergedProps();
                if (resDoc) {
                    mergedBody = c4doc_getProperties(resolvedDoc.doc());
                    mergedFlags |= resolvedDoc.doc()->selectedRev.flags;
                } else if (mergedProps) {
                    mergedBody = mergedProps;
                } else {
                    mergedFlags |= kRevDeleted;
                    mergedBody = kFLEmptyDict;
                }
            }
            
            alloc_slice winRevID = remoteDoc->selectedRev.revID;
            alloc_slice lostRevID = localDoc->selectedRev.revID;
            bool result = c4doc_resolveConflict2(localDoc, winRevID, lostRevID,
                                                 mergedBody, mergedFlags, &error);
            
            Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s' in '%.*s'",
                   SPLAT(rev->docID), SPLAT(collPath));
            Assert((localDoc->flags & kDocConflicted) == 0);
            
            if (!c4doc_save(localDoc, 0, &error)) {
                Assert(false, "conflictHandler: c4doc_save failed for '%.*s' in '%.*s'",
                       SPLAT(rev->docID), SPLAT(collPath));
            }
            conflictHandlerRunning = false;
        };
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
    ExpectingExceptions x;
    _expectedError = {LiteCoreDomain, kC4ErrorNotFound};
    runPushPullReplication(specs, specs);
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Unmatched Collections", "[Push][Pull]") {
    _expectedError = {WebSocketDomain, 404};
    runPushPullReplication({Roses, Lavenders}, {Tulips, Lavenders});
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Zero Collections", "[Push][Pull]") {
    ExpectingExceptions x;
    _expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
    runPushPullReplication({}, {});
}


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
    addDocs(db, Default, 10);
    addDocs(db2, Default, 10);
    
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

    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Default}, {Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    }
    
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

    SECTION("PUSH and PULL with MULTIPLE PASSIVE COLLECTIONS") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Default}, {Guitars, Default});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    }
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Single Collection", "[Push][Pull]") {
    addDocs(db, Guitars, 10);
    addDocs(db2, Guitars, 10);

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

    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Guitars}, {Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    }

    SECTION("PUSH with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPushReplication({Guitars}, {Default, Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    }

    SECTION("PULL with MULTIPLE PASSIVE COLLECTIONS") {
        CheckDBEntries check(db, db2, {Guitars});

        _expectedDocumentCount = 10;
        runPullReplication({Default, Guitars}, {Guitars});
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    }
    
    SECTION("PUSH and PULL with MULTIPLE PASSIVE COLLECTIONS") {
        _expectedDocumentCount = 20;
        runPushPullReplication({Guitars}, {Default, Guitars});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    }
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Multiple Collections", "[Push][Pull]") {
    addDocs(db, Roses, 10);
    addDocs(db, Tulips, 10);
    addDocs(db, Lavenders, 10);
    addDocs(db2, Roses, 20);
    addDocs(db2, Tulips, 20);
    addDocs(db2, Lavenders, 20);
    
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

    SECTION("PUSH and PULL") {
        _expectedDocumentCount = 60;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":20}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":20}");
    }

    SECTION("PUSH CONTINUOUS") {
        _expectedDocumentCount = 20;
        stopWhenIdle();
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    }

    SECTION("PULL CONTINUOUS") {
        _expectedDocumentCount = 20;
        stopWhenIdle();
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips}, kC4Continuous);
        validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
        validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    }
    
    SECTION("PUSH and PULL CONTINUOUS") {
        _expectedDocumentCount = 60;
        stopWhenIdle();
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        validateCollectionCheckpoints(db, db2, 0, "{\"local\":30,\"remote\":30}");
        validateCollectionCheckpoints(db, db2, 1, "{\"local\":30,\"remote\":30}");
    }
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Multiple Collections Incremental Push and Pull", "[Push][Pull]") {
    addDocs(db, Roses, 10);
    addDocs(db, Tulips, 10);
    addDocs(db2, Roses, 10);
    addDocs(db2, Tulips, 10);
    
    _expectedDocumentCount = 40;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10,\"remote\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10,\"remote\":10}");
    
    addDocs(db, Roses, 1, "rose1");
    addDocs(db, Tulips, 2, "tulip1");
    
    addDocs(db2, Roses, 3, "rose2");
    addDocs(db2, Tulips, 4, "tulip2");
    
    _expectedDocumentCount = 10;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":21,\"remote\":23}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":22,\"remote\":24}");
}

struct Jthread {
    std::thread thread;
    Jthread(std::thread&& thread_)
    : thread(move(thread_))
    {}
    Jthread() = default;
    ~Jthread() {
        thread.join();
    }
};

TEST_CASE_METHOD(ReplicatorCollectionTest, "Multiple Collections Incremental Revisions", "[Push][Pull]") {
    addDocs(db, Roses, 2, "db-Roses-");
    addDocs(db, Tulips, 2, "db-Tulips-");
    C4Collection* roses = getCollection(db, Roses);
    C4Collection* tulips = getCollection(db, Tulips);
    C4Collection* roses2 = getCollection(db2, Roses);
    C4Collection* tulips2 = getCollection(db2, Tulips);
    Jthread jthread;

    SECTION("PUSH") {
        _callbackWhenIdle = [=, &jthread]() {
            jthread.thread = std::thread(std::thread{[=]() {
                CHECK(c4coll_getDocumentCount(roses2) == 2);
                CHECK(c4coll_getDocumentCount(tulips2) == 2);

                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                sleepFor(1s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        // 4 docs plus 6 revs
        _expectedDocumentCount = 10;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);

        CHECK(c4coll_getDocumentCount(roses2) == 3);
        CHECK(c4coll_getDocumentCount(tulips2) == 3);
    }

    SECTION("PULL") {
        _callbackWhenIdle = [=, &jthread]() {
            jthread.thread = std::thread(std::thread{[=]() {
                CHECK(c4coll_getDocumentCount(roses2) == 2);
                CHECK(c4coll_getDocumentCount(tulips2) == 2);

                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                sleepFor(1s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        _expectedDocumentCount = 10;
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips}, kC4Continuous);
        CHECK(c4coll_getDocumentCount(roses2) == 3);
        CHECK(c4coll_getDocumentCount(tulips2) == 3);
    }

    SECTION("PUSH and PULL") {
        addDocs(db2, Roses, 2, "db2-Roses-");
        addDocs(db2, Tulips, 2, "db2-Tulips-");

        _callbackWhenIdle = [=, &jthread]() {
            jthread.thread = thread(std::thread{[=]() {
                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                addRevs(roses2, 500ms, alloc_slice("roses2-docko"), 1, 3, true, "db2-roses");
                addRevs(tulips2, 500ms, alloc_slice("tulips2-docko"), 1, 3, true, "db2-tulips");
                sleepFor(1s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        // 3 revs from roses to roses2, 3 from roses2 to roses,     total 6
        // 3 revs from tulips to tulips2, 3 from tulips2 to tulips, total 6
        // 4 docs for push, 4docs for pull,                         total 8
        _expectedDocumentCount = 20;
        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
        CHECK(c4coll_getDocumentCount(roses) == 6);
        CHECK(c4coll_getDocumentCount(tulips) == 6);
        CHECK(c4coll_getDocumentCount(roses2) == 6);
        CHECK(c4coll_getDocumentCount(tulips2) == 6);
    }
}

// Failed : CBL-3500
TEST_CASE_METHOD(ReplicatorCollectionTest, "Reset Checkpoint with Push", "[.CBL-3500]") {
    addDocs(db, Roses, 10);
    addDocs(db, Tulips, 10);
    
    _expectedDocumentCount = 20;
    runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    
    purgeAllDocs(db2, Roses);
    purgeAllDocs(db2, Tulips);
    
    _expectedDocumentCount = 0;
    runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
    
    _expectedDocumentCount = 20;
    runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4OneShot, true);
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":10}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":10}");
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Reset Checkpoint with Pull", "[Pull]") {
    addDocs(db, Roses, 10);
    addDocs(db, Tulips, 10);
    
    _expectedDocumentCount = 20;
    runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
    validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    
    purgeAllDocs(db2, Roses);
    purgeAllDocs(db2, Tulips);
    
    _expectedDocumentCount = 0;
    runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
    validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
    
    _expectedDocumentCount = 20;
    runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips}, kC4OneShot, true);
    validateCollectionCheckpoints(db2, db, 0, "{\"remote\":10}");
    validateCollectionCheckpoints(db2, db, 1, "{\"remote\":10}");
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Push and Pull Attachments", "[Push][Pull]") {
    vector<string> attachments1 = {"Attachment A", "Attachment B", "Attachment Z"};
    vector<C4BlobKey> blobKeys1a, blobKeys1b;
    {
        TransactionHelper t(db);
        blobKeys1a = addDocWithAttachments(db, Roses, "doc1"_sl, attachments1, "text/plain");
        blobKeys1b = addDocWithAttachments(db, Tulips, "doc2"_sl, attachments1, "text/plain");
    }
    
    vector<string> attachments2 = {"Attachment C", "Attachment D", "Attachment Z"};
    vector<C4BlobKey> blobKeys2a, blobKeys2b;
    {
        TransactionHelper t(db2);
        blobKeys2a = addDocWithAttachments(db2, Roses, "doc3"_sl, attachments2, "text/plain");
        blobKeys2b = addDocWithAttachments(db2, Tulips, "doc4"_sl, attachments2, "text/plain");
    }
    
    _expectedDocumentCount = 4;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    
    validateCollectionCheckpoints(db, db2, 0, "{\"local\":1,\"remote\":1}");
    validateCollectionCheckpoints(db, db2, 1, "{\"local\":1,\"remote\":1}");
    
    checkAttachments(db, blobKeys1a, attachments1);
    checkAttachments(db, blobKeys1b, attachments1);
    checkAttachments(db, blobKeys2a, attachments2);
    checkAttachments(db, blobKeys2b, attachments2);
    
    checkAttachments(db2, blobKeys1a, attachments1);
    checkAttachments(db2, blobKeys1b, attachments1);
    checkAttachments(db2, blobKeys2a, attachments2);
    checkAttachments(db2, blobKeys2b, attachments2);
}

TEST_CASE_METHOD(ReplicatorCollectionTest, "Resolve Conflict", "[Push][Pull]") {
    int resolveCount = 0;
    auto resolver = [&resolveCount](CollectionSpec spec, C4Document* localDoc, C4Document* remoteDoc) {
        resolveCount++;
        C4Document* resolvedDoc;
        if (spec == Roses) {
            resolvedDoc = remoteDoc;
        } else {
            resolvedDoc = localDoc;
        }
        return ResolvedDocument(resolvedDoc);
    };
    setConflictResolver(db2, resolver);
    
    auto roses1 = getCollection(db, Roses);
    auto tulips1 = getCollection(db, Tulips);
    
    auto roses2 = getCollection(db2, Roses);
    auto tulips2 = getCollection(db2, Tulips);
    
    // Create docs and push to the other db:
    createFleeceRev(roses1,  "rose1"_sl,  kRev1ID, "{}"_sl);
    createFleeceRev(tulips1, "tulip1"_sl, kRev1ID, "{}"_sl);
    
    _expectedDocumentCount = 2;
    runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    
    // Update docs on both dbs and run pull replication:
    createFleeceRev(roses1,  "rose1"_sl,  revOrVersID("2-12121212", "1@cafe"), "{\"db\":1}"_sl);
    createFleeceRev(roses2,  "rose1"_sl,  revOrVersID("2-13131313", "1@babe"), "{\"db\":2}"_sl);
    createFleeceRev(tulips1, "tulip1"_sl, revOrVersID("2-12121212", "1@cafe"), "{\"db\":1}"_sl);
    createFleeceRev(tulips2, "tulip1"_sl, revOrVersID("2-13131313", "1@babe"), "{\"db\":2}"_sl);
    
    // Pull from db (Passive) to db2 (Active)
    runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
    CHECK(resolveCount == 2);
    
    c4::ref<C4Document> doc1 = c4coll_getDoc(roses2, "rose1"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(fleece2json(c4doc_getRevisionBody(doc1)) == "{db:1}"); // Remote Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc1, true, false, nullptr));
    
    c4::ref<C4Document> doc2 = c4coll_getDoc(tulips2, "tulip1"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc2);
    CHECK(fleece2json(c4doc_getRevisionBody(doc2)) == "{db:2}"); // Local Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc2, true, false, nullptr));
}

#ifdef COUCHBASE_ENTERPRISE

struct CipherContext {
    C4Collection* collection;
    slice docID;
    slice keyPath;
    bool called;
};

using CipherContextMap = unordered_map<C4CollectionSpec, CipherContext*>;

static void validateCipherInputs(void* ctx, C4CollectionSpec& spec, C4String& docID, C4String& keyPath) {
    auto contextMap = (CipherContextMap*)ctx;
    auto i = contextMap->find(spec);
    REQUIRE(i != contextMap->end());
    
    auto context = i->second;
    CHECK(spec == context->collection->getSpec());
    CHECK(docID == context->docID);
    CHECK(keyPath == context->keyPath);
    
    context->called = true;
}

static C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError)
{
    validateCipherInputs(ctx, spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

static C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm,
                                   C4String keyID, C4Error* outError)
{
    validateCipherInputs(ctx, spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}


TEST_CASE_METHOD(ReplicatorCollectionTest, "Replicate Encrypted Properties with Collections", "[Push][Pull][Encryption]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));

    auto roses1 = getCollection(db, Roses);
    auto tulips1 = getCollection(db, Tulips);
    
    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    {
        TransactionHelper t(db);
        createFleeceRev(roses1, "hiddenRose"_sl, kRevID, originalJSON);
        createFleeceRev(tulips1, "invisibleTulip"_sl, kRevID, originalJSON);
        _expectedDocumentCount = 1;
    }
    
    CipherContextMap encContexts;
    CipherContext encContext1 = {roses1, "hiddenRose", "xNum", false};
    CipherContext encContext2 = {tulips1, "invisibleTulip", "xNum", false};
    encContexts[Roses]  = &encContext1;
    encContexts[Tulips] = &encContext2;
    
    _expectedDocumentCount = 2;
    auto opts = replicatorOptions({Roses, Tulips}, kC4OneShot, kC4Disabled);
    opts.propertyEncryptor = &propEncryptor;
    opts.propertyDecryptor = &propDecryptor;
    opts.callbackContext = &encContexts;
    
    auto roses2 = getCollection(db2, Roses);
    auto tulips2 = getCollection(db2, Tulips);
    
    CipherContextMap decContexts;
    CipherContext decContext1 = {roses2, "hiddenRose", "xNum", false};
    CipherContext decContext2 = {tulips2, "invisibleTulip", "xNum", false};
    decContexts[Roses]  = &decContext1;
    decContexts[Tulips] = &decContext2;

    auto serverOpts = replicatorOptions({Roses, Tulips}, kC4Passive, kC4Passive);
    serverOpts.propertyEncryptor = &propEncryptor;
    serverOpts.propertyDecryptor = &propDecryptor;
    serverOpts.callbackContext = &decContexts;
    
    if (!TestDecryption)
        serverOpts.setNoPropertyDecryption(); // default is true

    runReplicators(opts, serverOpts);
    
    // Check encryption on active replicator:
    for (auto i = encContexts.begin(); i != encContexts.end(); i++) {
        CipherContext* context = i->second;
        CHECK(context->called);
    }
    
    // Check decryption on passive replicator:
    for (auto i = decContexts.begin(); i != decContexts.end(); i++) {
        auto context = i->second;
        c4::ref<C4Document> doc = c4coll_getDoc(context->collection, context->docID, true,
                                                kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);
        
        if (TestDecryption) {
            CHECK(context->called);
            CHECK(props.toJSON(false, true) == originalJSON);
        } else {
            CHECK(!context->called);
            CHECK(props.toJSON(false, true) == R"({"encrypted$xNum":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

            // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
            // so the property won't be converted back from the server schema.
            slice cipherb64 = props["encrypted$xNum"].asDict()["ciphertext"].asString();
            auto cipher = base64::decode(cipherb64);
            alloc_slice clear = UnbreakableEncryption(cipher, -1);
            CHECK(clear == "\"123-45-6789\"");
        }
    }
}

#endif
