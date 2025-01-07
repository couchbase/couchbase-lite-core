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
#include "Defer.hh"
#include "fleece/Mutable.hh"
#include <future>

static constexpr slice            GuitarsName = "guitars"_sl;
static constexpr C4CollectionSpec Guitars     = {GuitarsName, kC4DefaultScopeID};

static constexpr slice RosesName        = "roses"_sl;
static constexpr slice TulipsName       = "tulips"_sl;
static constexpr slice LavenderName     = "lavenders"_sl;
static constexpr slice FlowersScopeName = "flowers"_sl;

static constexpr C4CollectionSpec Roses     = {RosesName, FlowersScopeName};
static constexpr C4CollectionSpec Tulips    = {TulipsName, FlowersScopeName};
static constexpr C4CollectionSpec Lavenders = {LavenderName, FlowersScopeName};
static constexpr C4CollectionSpec Default   = kC4DefaultCollectionSpec;

using namespace std;
using namespace litecore::repl;

using CollectionSpec    = C4Database::CollectionSpec;
using CollectionOptions = Options::CollectionOptions;

class ReplicatorCollectionTest : public ReplicatorLoopbackTest {
  public:
    ReplicatorCollectionTest(int which) : ReplicatorLoopbackTest(which) {
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
    void runPushReplication(vector<CollectionSpec> specs1, vector<CollectionSpec> specs2,
                            C4ReplicatorMode activeMode = kC4OneShot, bool reset = false) {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, activeMode, kC4Disabled);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2, reset);
    }

    // Pull from db1 to db2
    void runPullReplication(vector<CollectionSpec> specs1, vector<CollectionSpec> specs2,
                            C4ReplicatorMode activeMode = kC4OneShot, bool reset = false) {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, kC4Passive, kC4Passive);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Disabled, activeMode);
        runReplicationCollections(coll1, coll2, reset);
    }

    void runPushPullReplication(vector<CollectionSpec> specs1, vector<CollectionSpec> specs2,
                                C4ReplicatorMode activeMode = kC4OneShot, bool reset = false) {
        vector<C4ReplicationCollection> coll1 = replCollections(specs1, activeMode, activeMode);
        vector<C4ReplicationCollection> coll2 = replCollections(specs2, kC4Passive, kC4Passive);
        runReplicationCollections(coll1, coll2, reset);
    }

    void runReplicationCollections(vector<C4ReplicationCollection>& coll1, vector<C4ReplicationCollection>& coll2,
                                   bool reset = false) {
        C4ReplicatorParameters params1{};
        params1.collectionCount = coll1.size();
        if ( !coll1.empty() ) { params1.collections = coll1.data(); }
        Options opts1 = Options(params1);

        C4ReplicatorParameters params2{};
        params2.collectionCount = coll2.size();
        if ( !coll2.empty() ) { params2.collections = coll2.data(); }
        Options opts2 = Options(params2);

        runReplicators(opts1, opts2, reset);
    }

    static Options replicatorOptions(vector<CollectionSpec> specs, C4ReplicatorMode pushMode,
                                     C4ReplicatorMode pullMode) {
        vector<C4ReplicationCollection> coll = replCollections(specs, pushMode, pullMode);
        C4ReplicatorParameters          params{};
        params.collectionCount = coll.size();
        if ( !coll.empty() ) { params.collections = coll.data(); }
        return Options(params);
    }

    static vector<CollectionSpec> getCollectionSpecs(C4Database* db, slice scope) {
        vector<CollectionSpec> specs;
        db->forEachCollection(scope, [&](C4CollectionSpec spec) { specs.emplace_back(spec); });
        return specs;
    }

    void purgeAllDocs(C4Database* db, CollectionSpec spec) {
        C4Collection* coll = getCollection(db, spec);

        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        options.flags &= ~kC4IncludeBodies;

        C4Error error;
        auto    e = c4coll_enumerateAllDocs(coll, &options, ERROR_INFO(error));
        REQUIRE(e);
        {
            TransactionHelper t(db2);
            while ( c4enum_next(e, &error) ) {
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
        ResolvedDocument() = default;  // Resolved as a deleted doc

        explicit ResolvedDocument(C4Document* doc) : _doc(c4doc_retain(doc)) {}

        explicit ResolvedDocument(FLDict mergedProps) : _mergedProps(mergedProps) {}

        C4Document* doc() { return _doc; }

        FLDict mergedProps() { return _mergedProps; }

      private:
        c4::ref<C4Document> _doc;
        RetainedDict        _mergedProps;
    };

    void setConflictResolver(C4Database*                                                activeDB,
                             const std::function<ResolvedDocument(CollectionSpec collection, C4Document* local,
                                                                  C4Document* remote)>& resolver) {
        REQUIRE(activeDB);

        if ( !resolver ) {
            _conflictHandler = nullptr;
            return;
        }

        auto& conflictHandlerRunning = _conflictHandlerRunning;
        _conflictHandler             = [activeDB, resolver, &conflictHandlerRunning](ReplicatedRev* rev) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            auto collPath = Options::collectionSpecToPath(rev->collectionSpec);
            Log("Resolving conflict for '%.*s' in '%.*s' ...", SPLAT(rev->docID), SPLAT(collPath));

            C4Error       error;
            C4Collection* coll = c4db_getCollection(activeDB, rev->collectionSpec, &error);
            Assert(coll, "conflictHandler: Couldn't find collection '%.*s'", SPLAT(collPath));

            conflictHandlerRunning = true;
            TransactionHelper t(activeDB);

            // Get the local doc:
            c4::ref<C4Document> localDoc = c4coll_getDoc(coll, rev->docID, true, kDocGetAll, &error);
            Assert(localDoc, "conflictHandler: Couldn't read doc '%.*s' in '%.*s'", SPLAT(rev->docID), SPLAT(collPath));

            // Get the remote doc:
            c4::ref<C4Document> remoteDoc = c4coll_getDoc(coll, rev->docID, true, kDocGetAll, &error);
            if ( !c4doc_selectNextLeafRevision(remoteDoc, true, false, &error) ) {
                Assert(false, "conflictHandler: Couldn't get conflicting remote revision of '%.*s' in '%.*s'",
                                   SPLAT(rev->docID), SPLAT(collPath));
            }

            ResolvedDocument resolvedDoc;
            if ( (localDoc->selectedRev.flags & kRevDeleted) && (remoteDoc->selectedRev.flags & kRevDeleted) ) {
                resolvedDoc = ResolvedDocument(remoteDoc);
            } else {
                resolvedDoc = resolver(coll->getSpec(), localDoc.get(), remoteDoc.get());
            }

            FLDict          mergedBody  = nullptr;
            C4RevisionFlags mergedFlags = 0;

            if ( resolvedDoc.doc() == remoteDoc ) {
                mergedFlags |= resolvedDoc.doc()->selectedRev.flags;
            } else {
                C4Document* resDoc      = resolvedDoc.doc();
                FLDict      mergedProps = resolvedDoc.mergedProps();
                if ( resDoc ) {
                    mergedBody = c4doc_getProperties(resolvedDoc.doc());
                    mergedFlags |= resolvedDoc.doc()->selectedRev.flags;
                } else if ( mergedProps ) {
                    mergedBody = mergedProps;
                } else {
                    mergedFlags |= kRevDeleted;
                    mergedBody = kFLEmptyDict;
                }
            }

            alloc_slice winRevID  = remoteDoc->selectedRev.revID;
            alloc_slice lostRevID = localDoc->selectedRev.revID;
            bool        result = c4doc_resolveConflict2(localDoc, winRevID, lostRevID, mergedBody, mergedFlags, &error);

            Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s' in '%.*s'", SPLAT(rev->docID),
                               SPLAT(collPath));
            Assert((localDoc->flags & kDocConflicted) == 0);

            if ( !c4doc_save(localDoc, 0, &error) ) {
                Assert(false, "conflictHandler: c4doc_save failed for '%.*s' in '%.*s'", SPLAT(rev->docID),
                                   SPLAT(collPath));
            }
            conflictHandlerRunning = false;
        };
    }

  protected:
    ~ReplicatorCollectionTest() override {
        // Use this to test binary log encoding/decoding is working (make sure you are running with '-r quiet')
        if ( getenv("TEST_BINARY_LOGS") ) { CHECK(false); }
    }

  private:
    static vector<C4ReplicationCollection> replCollections(vector<CollectionSpec>& specs, C4ReplicatorMode pushMode,
                                                           C4ReplicatorMode pullMode) {
        vector<C4ReplicationCollection> colls(specs.size());
        for ( int i = 0; i < specs.size(); i++ ) {
            colls[i].collection = specs[i];
            colls[i].push       = pushMode;
            colls[i].pull       = pullMode;
        }
        return colls;
    }
};

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Nonexisting Collections", "[Push][Pull]") {
    vector<CollectionSpec> specs = {CollectionSpec("dummy1"_sl), CollectionSpec("dummy2"_sl)};
    ExpectingExceptions    x;
    _expectedError = {LiteCoreDomain, kC4ErrorNotFound};
    runPushPullReplication(specs, specs);
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Unmatched Collections", "[Push][Pull]") {
    _expectedError = {WebSocketDomain, 404};
    runPushPullReplication({Roses, Lavenders}, {Tulips, Lavenders});
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Use Zero Collections", "[Push][Pull]") {
    ExpectingExceptions x;
    _expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
    runPushPullReplication({}, {});
}

static std::set<string> getDocInfos(C4Database* db, C4CollectionSpec coll) {
    std::set<string> ret;
    C4Collection*    collection = ReplicatorCollectionTest::getCollection(db, coll);
    auto             e          = c4coll_enumerateAllDocs(collection, nullptr, ERROR_INFO());
    {
        TransactionHelper t(db);
        while ( c4enum_next(e, ERROR_INFO()) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            alloc_slice revID = db->getRevIDGlobalForm(info.revID);
            string      entry = slice(info.docID).asString() + "/" + revID.asString();
            ret.insert(entry);
        }
    }
    c4enum_free(e);
    return ret;
}

struct CheckDBEntries {
    CheckDBEntries(C4Database* db, C4Database* db2, const vector<C4CollectionSpec>& specs) : _db(db), _db2(db2) {
        for ( auto& spec : specs ) {
            _collSpecs.push_back(spec);
            _dbBefore.push_back(getDocInfos(_db, spec));
            _db2Before.push_back(getDocInfos(_db2, spec));
        }
    }

    ~CheckDBEntries() {
        vector<set<string>> dbAfter;
        vector<set<string>> db2After;
        for ( auto i = 0; i < _collSpecs.size(); ++i ) {
            dbAfter.push_back(getDocInfos(_db, _collSpecs[i]));
            db2After.push_back(getDocInfos(_db2, _collSpecs[i]));
            CHECK(dbAfter[i].size() == _dbBefore[i].size());
            for ( auto& doc : _dbBefore[i] ) {
                INFO("Checking doc " << doc << " from db is in db2");
                CHECK(db2After[i].erase(doc) == 1);
            }
            for ( auto& doc : _db2Before[i] ) {
                INFO("Checking doc " << doc << " from db2 is in db2");
                CHECK(db2After[i].erase(doc) == 1);
            }
            CHECK(db2After[i].size() == 0);
        }
    }

    C4Database*              _db;
    C4Database*              _db2;
    vector<C4CollectionSpec> _collSpecs;
    vector<set<string>>      _dbBefore;
    vector<set<string>>      _db2Before;
};

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Default Collection", "[Push][Pull]") {
#ifdef LITECORE_CPPTEST
    bool collectionAwareActive  = GENERATE(false, true);
    bool collectionAwareOnEntry = repl::Options::sActiveIsCollectionAware;
    if ( collectionAwareActive ) {
        repl::Options::sActiveIsCollectionAware = true;
        std::cerr << "        Active Replicator is collection-aware" << std::endl;
    }
    DEFER { repl::Options::sActiveIsCollectionAware = collectionAwareOnEntry; };
#endif

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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":10})");
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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":10})");
    }
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Single Collection", "[Push][Pull]") {
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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":10})");
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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":10})");
    }
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Sync with Multiple Collections", "[Push][Pull]") {
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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":20})");
        validateCollectionCheckpoints(db, db2, 1, R"({"local":10,"remote":20})");
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
        validateCollectionCheckpoints(db, db2, 0, R"({"local":30,"remote":30})");
        validateCollectionCheckpoints(db, db2, 1, R"({"local":30,"remote":30})");
    }
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Multiple Collections Incremental Push and Pull", "[Push][Pull]") {
    addDocs(db, Roses, 10);
    addDocs(db, Tulips, 10);
    addDocs(db2, Roses, 10);
    addDocs(db2, Tulips, 10);

    _expectedDocumentCount = 40;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, R"({"local":10,"remote":10})");
    validateCollectionCheckpoints(db, db2, 1, R"({"local":10,"remote":10})");

    addDocs(db, Roses, 1, "rose1");
    addDocs(db, Tulips, 2, "tulip1");

    addDocs(db2, Roses, 3, "rose2");
    addDocs(db2, Tulips, 4, "tulip2");

    _expectedDocumentCount = 10;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});
    validateCollectionCheckpoints(db, db2, 0, R"({"local":21,"remote":23})");
    validateCollectionCheckpoints(db, db2, 1, R"({"local":22,"remote":24})");
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Multiple Collections Incremental Revisions", "[Push][Pull]") {
    addDocs(db, Roses, 2, "db-Roses-");
    addDocs(db, Tulips, 2, "db-Tulips-");
    C4Collection* roses   = getCollection(db, Roses);
    C4Collection* tulips  = getCollection(db, Tulips);
    C4Collection* roses2  = getCollection(db2, Roses);
    C4Collection* tulips2 = getCollection(db2, Tulips);
    Jthread       jthread;
    _expectedDocumentCount                                                    = -1;
    std::vector<std::pair<C4Collection*, slice>> docsWithIncrementalRevisions = {{roses2, "roses-docko"_sl},
                                                                                 {tulips2, "tulips-docko"_sl}};

    SECTION("PUSH") {
        _callbackWhenIdle = [this, &jthread, roses, tulips, roses2, tulips2]() {
            jthread.thread    = std::thread(std::thread{[this, roses, tulips, roses2, tulips2]() {
                CHECK(c4coll_getDocumentCount(roses2) == 2);
                CHECK(c4coll_getDocumentCount(tulips2) == 2);

                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                sleepFor(1s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
    }
    SECTION("PULL") {
        _callbackWhenIdle = [this, &jthread, roses, tulips, roses2, tulips2]() {
            jthread.thread    = std::thread(std::thread{[this, roses, tulips, roses2, tulips2]() {
                CHECK(c4coll_getDocumentCount(roses2) == 2);
                CHECK(c4coll_getDocumentCount(tulips2) == 2);

                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                sleepFor(1s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips}, kC4Continuous);
    }
    SECTION("PUSH and PULL") {
        addDocs(db2, Roses, 2, "db2-Roses-");
        addDocs(db2, Tulips, 2, "db2-Tulips-");
        docsWithIncrementalRevisions.emplace_back(roses, "roses2-docko"_sl);
        docsWithIncrementalRevisions.emplace_back(tulips, "tulips2-docko"_sl);

        _callbackWhenIdle = [this, &jthread, roses, tulips, roses2, tulips2]() {
            jthread.thread    = thread(std::thread{[this, roses, tulips, roses2, tulips2]() {
                // When first time it turns to Idle, we assume 2 documents from db are pushed to db2,
                // and 2 documents from db2 are pulled to db.
                CHECK(c4coll_getDocumentCount(roses) == 4);
                CHECK(c4coll_getDocumentCount(tulips) == 4);
                CHECK(c4coll_getDocumentCount(roses2) == 4);
                CHECK(c4coll_getDocumentCount(tulips2) == 4);

                // We now add 3 revisions of respective docs to db and db2. The are supposed to be
                // pushed and pulled to db2 and db, respectively.
                // In 5 seconds, we assume that latest revision, 3, will be replicated to the
                // destinations.
                addRevs(roses, 500ms, alloc_slice("roses-docko"), 1, 3, true, "db-roses");
                addRevs(tulips, 500ms, alloc_slice("tulips-docko"), 1, 3, true, "db-tulips");
                addRevs(roses2, 500ms, alloc_slice("roses2-docko"), 1, 3, true, "db2-roses");
                addRevs(tulips2, 500ms, alloc_slice("tulips2-docko"), 1, 3, true, "db2-tulips");
                sleepFor(5s);
                stopWhenIdle();
            }});
            _callbackWhenIdle = nullptr;
        };

        runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses}, kC4Continuous);
    }

    // Check docs that have incremental revisions got across the latest revision, 3.
    for ( const auto& coll_doc : docsWithIncrementalRevisions ) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll_doc.first, coll_doc.second, true, kDocGetMetadata, ERROR_INFO());
        CHECK(doc);
        if ( doc && isRevTrees() ) CHECK(c4rev_getGeneration(doc->revID) == 3);
    }
}

// Failed : CBL-3500
N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Reset Checkpoint with Push", "[.CBL-3500]") {
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

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Reset Checkpoint with Pull", "[Pull]") {
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

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Push and Pull Attachments", "[Push][Pull]") {
    vector<string>    attachments1 = {"Attachment A", "Attachment B", "Attachment Z"};
    vector<C4BlobKey> blobKeys1a, blobKeys1b;
    {
        TransactionHelper t(db);
        blobKeys1a = addDocWithAttachments(db, Roses, "doc1"_sl, attachments1, "text/plain");
        blobKeys1b = addDocWithAttachments(db, Tulips, "doc2"_sl, attachments1, "text/plain");
    }

    vector<string>    attachments2 = {"Attachment C", "Attachment D", "Attachment Z"};
    vector<C4BlobKey> blobKeys2a, blobKeys2b;
    {
        TransactionHelper t(db2);
        blobKeys2a = addDocWithAttachments(db2, Roses, "doc3"_sl, attachments2, "text/plain");
        blobKeys2b = addDocWithAttachments(db2, Tulips, "doc4"_sl, attachments2, "text/plain");
    }

    _expectedDocumentCount = 4;
    runPushPullReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});

    validateCollectionCheckpoints(db, db2, 0, R"({"local":1,"remote":1})");
    validateCollectionCheckpoints(db, db2, 1, R"({"local":1,"remote":1})");

    checkAttachments(db, blobKeys1a, attachments1);
    checkAttachments(db, blobKeys1b, attachments1);
    checkAttachments(db, blobKeys2a, attachments2);
    checkAttachments(db, blobKeys2b, attachments2);

    checkAttachments(db2, blobKeys1a, attachments1);
    checkAttachments(db2, blobKeys1b, attachments1);
    checkAttachments(db2, blobKeys2a, attachments2);
    checkAttachments(db2, blobKeys2b, attachments2);
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Resolve Conflict", "[Push][Pull]") {
    int  resolveCount = 0;
    auto resolver     = [&resolveCount](CollectionSpec spec, C4Document* localDoc, C4Document* remoteDoc) {
        resolveCount++;
        C4Document* resolvedDoc;
        if ( spec == Roses ) {
            resolvedDoc = remoteDoc;
        } else {
            resolvedDoc = localDoc;
        }
        return ResolvedDocument(resolvedDoc);
    };
    setConflictResolver(db2, resolver);

    auto roses1  = getCollection(db, Roses);
    auto tulips1 = getCollection(db, Tulips);

    auto roses2  = getCollection(db2, Roses);
    auto tulips2 = getCollection(db2, Tulips);

    // Create docs and push to the other db:
    createFleeceRev(roses1, "rose1"_sl, kRev1ID, "{}"_sl);
    createFleeceRev(tulips1, "tulip1"_sl, kRev1ID, "{}"_sl);

    _expectedDocumentCount = 2;
    runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});

    // Update docs on both dbs and run pull replication:
    createFleeceRev(roses1, "rose1"_sl, revOrVersID("2-12121212", "1@CarolCarolCarolCarolCA"), "{\"db\":1}"_sl);
    createFleeceRev(roses2, "rose1"_sl, revOrVersID("2-13131313", "1@BobBobBobBobBobBobBobA"), "{\"db\":2}"_sl);
    createFleeceRev(tulips1, "tulip1"_sl, revOrVersID("2-12121212", "1@CarolCarolCarolCarolCA"), "{\"db\":1}"_sl);
    createFleeceRev(tulips2, "tulip1"_sl, revOrVersID("2-13131313", "1@BobBobBobBobBobBobBobA"), "{\"db\":2}"_sl);

    // Pull from db (Passive) to db2 (Active)
    runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});
    CHECK(resolveCount == 2);

    c4::ref<C4Document> doc1 = c4coll_getDoc(roses2, "rose1"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(fleece2json(c4doc_getRevisionBody(doc1)) == "{db:1}");  // Remote Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc1, true, false, nullptr));

    c4::ref<C4Document> doc2 = c4coll_getDoc(tulips2, "tulip1"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc2);
    CHECK(fleece2json(c4doc_getRevisionBody(doc2)) == "{db:2}");  // Local Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc2, true, false, nullptr));
}

#ifdef COUCHBASE_ENTERPRISE

struct CipherContext {
    C4Collection* collection{};
    slice         docID;
    slice         keyPath;
    bool          called{};
};

using CipherContextMap = unordered_map<C4CollectionSpec, CipherContext*>;

static mutex sCatchMutex;

static void validateCipherInputs(void* ctx, C4CollectionSpec& spec, C4String& docID, C4String& keyPath) {
    unique_lock lock(sCatchMutex);  // I may be called on multiple threads, but Catch is not thread-safe

    auto contextMap = (CipherContextMap*)ctx;
    auto i          = contextMap->find(spec);
    REQUIRE(i != contextMap->end());

    auto context = i->second;
    CHECK(spec == context->collection->getSpec());
    CHECK(docID == context->docID);
    CHECK(keyPath == context->keyPath);

    context->called = true;
}

static C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError) {
    validateCipherInputs(ctx, spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

static C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm, C4String keyID,
                                   C4Error* outError) {
    validateCipherInputs(ctx, spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Replicate Encrypted Properties with Collections",
                       "[Push][Pull][Encryption]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));

    auto roses1  = getCollection(db, Roses);
    auto tulips1 = getCollection(db, Tulips);

    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    {
        TransactionHelper t(db);
        createFleeceRev(roses1, "hiddenRose"_sl, kRevID, originalJSON);
        createFleeceRev(tulips1, "invisibleTulip"_sl, kRevID, originalJSON);
        _expectedDocumentCount = 1;
    }

    CipherContextMap encContexts;
    CipherContext    encContext1 = {roses1, "hiddenRose", "xNum", false};
    CipherContext    encContext2 = {tulips1, "invisibleTulip", "xNum", false};
    encContexts[Roses]           = &encContext1;
    encContexts[Tulips]          = &encContext2;

    _expectedDocumentCount = 2;
    auto opts              = replicatorOptions({Roses, Tulips}, kC4OneShot, kC4Disabled);
    opts.propertyEncryptor = &propEncryptor;
    opts.propertyDecryptor = &propDecryptor;
    opts.callbackContext   = &encContexts;

    auto roses2  = getCollection(db2, Roses);
    auto tulips2 = getCollection(db2, Tulips);

    CipherContextMap decContexts;
    CipherContext    decContext1 = {roses2, "hiddenRose", "xNum", false};
    CipherContext    decContext2 = {tulips2, "invisibleTulip", "xNum", false};
    decContexts[Roses]           = &decContext1;
    decContexts[Tulips]          = &decContext2;

    auto serverOpts              = replicatorOptions({Roses, Tulips}, kC4Passive, kC4Passive);
    serverOpts.propertyEncryptor = &propEncryptor;
    serverOpts.propertyDecryptor = &propDecryptor;
    serverOpts.callbackContext   = &decContexts;

    if ( !TestDecryption ) serverOpts.setNoPropertyDecryption();  // default is true

    runReplicators(opts, serverOpts);

    // Check encryption on active replicator:
    for ( auto& encContext : encContexts ) {
        CipherContext* context = encContext.second;
        CHECK(context->called);
    }

    // Check decryption on passive replicator:
    for ( auto& decContext : decContexts ) {
        auto                context = decContext.second;
        c4::ref<C4Document> doc = c4coll_getDoc(context->collection, context->docID, true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);

        if ( TestDecryption ) {
            CHECK(context->called);
            CHECK(props.toJSON(false, true) == originalJSON);
        } else {
            CHECK(!context->called);
            CHECK(props.toJSON(false, true)
                  == R"({"encrypted$xNum":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

            // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
            // so the property won't be converted back from the server schema.
            slice       cipherb64 = props["encrypted$xNum"].asDict()["ciphertext"].asString();
            auto        cipher    = base64::decode(cipherb64);
            alloc_slice clear     = UnbreakableEncryption(cipher, -1);
            CHECK(clear == "\"123-45-6789\"");
        }
    }
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Filters & docIDs with Multiple Collections", "[Sync][Filters]") {
    string db_roses   = "db-roses-";
    string db_tulips  = "db-tulips-";
    string db2_roses  = "db2-roses-";
    string db2_tulips = "db2-tulips-";
    addDocs(db, Roses, 10, db_roses);
    addDocs(db, Tulips, 10, db_tulips);
    addDocs(db, Lavenders, 10);
    addDocs(db2, Roses, 20, db2_roses);
    addDocs(db2, Tulips, 20, db2_tulips);
    addDocs(db2, Lavenders, 20);

    SECTION("PUSH") {
        C4ReplicatorValidationFunction pushFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                                                       C4RevisionFlags, FLDict body, void* context) {
            CHECK(collectionSpec == Roses);
            slice drop{(const char*)context};
            return drop != docID;
        };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                // Assign pushFilter to Roses
                if ( o.collectionSpec == Roses ) {
                    o.pushFilter      = pushFilter;
                    o.callbackContext = (void*)"db-roses-1";
                }
            }
            return ret;
        };

        // db is the active Push replicator.
        // A Push filter is applied to Roses. It lets pass all docs but one, "db-roses-1",
        // from db to db2
        _expectedDocumentCount = 19;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});

        C4Collection*       roses2  = getCollection(db2, Roses);
        C4Collection*       tulips2 = getCollection(db2, Tulips);
        c4::ref<C4Document> rose1   = c4coll_getDoc(roses2, "db-roses-1"_sl, true, kDocGetMetadata, ERROR_INFO());
        c4::ref<C4Document> tulip1  = c4coll_getDoc(tulips2, "db-tulips-1"_sl, true, kDocGetMetadata, ERROR_INFO());

        CHECK(!rose1);
        CHECK(tulip1);
    }

    SECTION("PULL") {
        C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                                                       C4RevisionFlags, FLDict body, void* context) {
            CHECK(collectionSpec == Tulips);
            slice drop{(const char*)context};
            return drop != docID;
        };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                // Assign pullFilter to Tulips
                if ( o.collectionSpec == Tulips ) {
                    o.pullFilter      = pullFilter;
                    o.callbackContext = (void*)"db-tulips-1";
                }
            }
            return ret;
        };

        // db2 is the active Pull replicator.
        // A pull filter is applied to collection Tulips. It requests to pull all docs from
        // db except for "db-tulips-1".
        _expectedDocumentCount = 19;
        // pull filter will generate errors for failed documents.
        _expectedDocPullErrors = set<string>{"db-tulips-1"};
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});

        C4Collection*       roses2  = getCollection(db2, Roses);
        C4Collection*       tulips2 = getCollection(db2, Tulips);
        c4::ref<C4Document> rose1   = c4coll_getDoc(roses2, "db-roses-1"_sl, true, kDocGetMetadata, ERROR_INFO());
        c4::ref<C4Document> tulip1  = c4coll_getDoc(tulips2, "db-tulips-1"_sl, true, kDocGetMetadata, ERROR_INFO());

        CHECK(rose1);
        CHECK(!tulip1);
    }

    SECTION("DocIDs on PULL") {
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString("db-tulips-2"_sl);
        enc.writeString("db-tulips-7"_sl);
        enc.writeString("db-tulips-4"_sl);
        enc.endArray();
        Doc docIDs{enc.finish()};
        _updateClientOptions = [&](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                if ( o.collectionSpec == Tulips ) { o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root()); }
            }
            return ret;
        };

        // db2 is the active replicator. Only 3 documents are specifiec in docIDs for the Tulips
        // collection, for a total of 13.
        _expectedDocumentCount = 13;
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});

        // db2 is the active client.
        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        // All 10 docs in Roses are pulled to db2
        CHECK(c4coll_getDocumentCount(roses2) == 30);
        // Only 3 docs in Tulips are pulled to db2
        CHECK(c4coll_getDocumentCount(tulips2) == 23);
    }

    SECTION("DocIDs & Filter on PULL") {
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString("db-tulips-2"_sl);
        enc.writeString("db-tulips-7"_sl);
        enc.writeString("db-tulips-4"_sl);
        enc.endArray();
        Doc                            docIDs{enc.finish()};
        C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                                                       C4RevisionFlags, FLDict body, void* context) {
            // filters are applied after docIDs
            CHECK((docID == "db-tulips-2"_sl || docID == "db-tulips-4"_sl || docID == "db-tulips-7"_sl));
            return docID != "db-tulips-4"_sl;
        };
        _updateClientOptions = [&](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                if ( o.collectionSpec == Tulips ) {
                    o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root());
                    o.pullFilter = pullFilter;
                }
            }
            return ret;
        };

        // db2 is the active pull replicator. Both docIDs and pull filter are applied to the Tulips
        // collecton. docIDs includes 3 documments. pullFilter rejects one among the three, giving rise
        // to a total of 12 to be pulled from db to db2.
        _expectedDocumentCount = 12;
        // docIDs takes precedence. The pull filter only receives the docs from docIDs, and
        // "db-tulips-4" fails the filter.
        _expectedDocPullErrors = set<string>{"db-tulips-4"};
        runPullReplication({Tulips, Lavenders, Roses}, {Roses, Tulips});

        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        // All 10 docs in Roses are pulled to db2
        CHECK(c4coll_getDocumentCount(roses2) == 30);
        // Only 2 docs in Tulips are pulled to db2
        CHECK(c4coll_getDocumentCount(tulips2) == 22);
    }

    SECTION("DocIDs on PUSH") {
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString("db-roses-2"_sl);
        enc.writeString("db-roses-7"_sl);
        enc.writeString("db-roses-4"_sl);
        enc.endArray();
        Doc docIDs{enc.finish()};

        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                if ( o.collectionSpec == Roses ) { o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root()); }
            }
            return ret;
        };

        // db is the active Push filter.
        // Only 3 documents are specified in docIDs for the Roses collection, for a total of 13.
        _expectedDocumentCount = 13;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});

        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        // Only 3 docs in Roese are pushed to db2
        CHECK(c4coll_getDocumentCount(roses2) == 23);
        // All 10 docs in Tulips are pushed to db2
        CHECK(c4coll_getDocumentCount(tulips2) == 30);
    }

    SECTION("DocIDs & Filter on PUSH") {
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString("db-roses-2"_sl);
        enc.writeString("db-roses-7"_sl);
        enc.writeString("db-roses-4"_sl);
        enc.endArray();
        Doc docIDs{enc.finish()};

        C4ReplicatorValidationFunction pushFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                                                       C4RevisionFlags, FLDict body, void* context) {
            CHECK((docID == "db-roses-2"_sl || docID == "db-roses-4"_sl || docID == "db-roses-7"_sl));
            return docID != "db-roses-4"_sl;
        };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for ( repl::Options::CollectionOptions& o : ret.collectionOpts ) {
                if ( o.collectionSpec == Roses ) {
                    o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root());
                    o.pushFilter = pushFilter;
                }
            }
            return ret;
        };

        // db is the active push replicator. Both docIDs and push filter are applied to the Roses
        // collecton. docIDs includes 3 documments. pushFilter rejects one among the three, giving rise
        // to a total of 12 to be pushed from db to db2.
        _expectedDocumentCount = 12;
        runPushReplication({Roses, Tulips}, {Tulips, Lavenders, Roses});

        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        // Only 2 docs in Roses are pushed to db2
        CHECK(c4coll_getDocumentCount(roses2) == 22);
        // All 10 docs in Tulips are pushed to db2
        CHECK(c4coll_getDocumentCount(tulips2) == 30);
    }
}

N_WAY_TEST_CASE_METHOD(ReplicatorCollectionTest, "Remote RevID Continuous Push", "[Push]") {
    // 1. Create 1 doc
    // 2. Start a continuous push replicator
    // 3. Wait until idle
    // 4. Update the doc.
    // 5. Wait until idle and stop
    // 6. Check the log if the proposeChange contains the remoteRevID when the update is push
    C4Collection* roses = getCollection(db, Roses);
    {
        auto              body = json2fleece("{'ok':'really!'}");
        TransactionHelper t(db);
        C4DocPutRequest   rq = {};
        rq.body              = body;
        rq.docID             = slice("doc1");
        rq.revFlags          = 0;
        rq.save              = true;
        C4Error c4err;
        auto    doc = c4coll_putDoc(roses, &rq, nullptr, &c4err);
        c4doc_release(doc);
    }

    std::future<void> future;
    _callbackWhenIdle = [this, roses, &future]() {
        _callbackWhenIdle        = nullptr;
        c4::ref<C4Document> doc1 = c4coll_getDoc(roses, slice("doc1"), true, kDocGetAll, ERROR_INFO());
        TransactionHelper   t(db);
        c4::ref<C4Document> doc = c4doc_update(doc1, json2fleece("{'ok':'no way!'}"), 0, nullptr);
        future                  = std::async(std::launch::async, [this]() {
            sleepFor(1s);
            stopWhenIdle();
        });
    };

    _expectedDocumentCount = 2;
    runPushReplication({Roses}, {Roses}, kC4Continuous);
}

#endif
