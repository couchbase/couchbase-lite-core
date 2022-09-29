//
//  ReplicatorCollectionSGTest.cc
//
//  Copyright 2022-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "c4Base.h"
#include "ReplicatorAPITest.hh"
#include "ReplicatorOptions.hh"
#include "ReplicatorLoopbackTest.hh"
#include "Base64.hh"
#include "Defer.hh"
#include "c4Collection.h"
#include "c4DocEnumerator.h"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Replicator.hh"
#include "fleece/Mutable.hh"
#include <array>

// Tests in this file, tagged by [.SyncServerCollection], are not done automatically in the
// Jenkins/GitHub CI. They can be run in locally with the following environment.
// Couchbase DB server, with docker, for example,
//   docker run -d --name cbserver -p 8091-8096:8091-8096 -p 11210-11211:11210-11211 couchbase:7.1.1
//   bucket configuration:
//     user    : Administrator
//     password: password
//     name    : any
//     scope   : flowers
//     collection: roses
// Sync-gateway:
//   config.json:
/*
 {
   "bootstrap": {
     "server": "couchbase://localhost",
     "username": "Administrator",
     "password": "password",
     "use_tls_server": false
   },
   "logging": {
     "console": {
       "log_level": "info",
       "log_keys": ["*"]
     }
   }
 }
 */
//  config db:
/*
 curl --location --request PUT "localhost:4985/scratch/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"num_index_replicas": 0, "bucket": "your_bucket_name", "scopes": {"flowers": {"collections":{"flowers":{}}}}}'
 */
//  config SG user:
/*
 curl --location --request POST "localhost:4985/scratch/_user/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"name": "sguser", "password": "password", "admin_channels": ["*"]}'
 */
//
// command argument:
//   [.SyncServerCollection]
//

static constexpr slice GuitarsName = "guitars"_sl;
static constexpr C4CollectionSpec Guitars = { GuitarsName, kC4DefaultScopeID };

static constexpr slice RosesName = "roses"_sl;
static constexpr slice TulipsName = "tulips"_sl;
//static constexpr slice LavenderName = "lavenders"_sl;
static constexpr slice FlowersScopeName = "flowers"_sl;

static constexpr C4CollectionSpec Roses = { RosesName, FlowersScopeName };
static constexpr C4CollectionSpec Tulips = { TulipsName, FlowersScopeName };
//static constexpr C4CollectionSpec Lavenders = { LavenderName, FlowersScopeName };
static constexpr C4CollectionSpec Default = kC4DefaultCollectionSpec;

using namespace std;
using namespace litecore::repl;

static C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError);

static C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm,
                                   C4String keyID, C4Error* outError);

class ReplicatorCollectionSGTest : public ReplicatorAPITest {
public:
    ~ReplicatorCollectionSGTest() {
        if (verifyDb != nullptr) {
            bool deletedDb = c4db_delete(verifyDb, ERROR_INFO());
            REQUIRE(deletedDb);
            c4db_release(verifyDb);
            verifyDb = nullptr;
        }
    }

    // Database verifyDb:
    C4Database* verifyDb {nullptr};
    void resetVerifyDb() {
        if (verifyDb == nullptr) {
            verifyDb = createDatabase("verifyDb");
        } else {
            deleteAndRecreateDB(verifyDb);
        }
    }

    // This function should be called before replicating against the Couchbase server.
    // It does the following:
    //  - sets up _options for authenticaton
    //  - assigns _collection with input "collection"
    //  - creates the collection if it is not the default collection
    //  - sets up the log level with input "logLevel"
    //  - returns the C4Collection object.
    template<size_t N>
    std::array<C4Collection*, N>
    collectionPreamble(std::array<C4CollectionSpec, N> collections,
                            const char* user, const char* password,
                            C4LogLevel logLevel =kC4LogNone) {
        // Setup Replicator Options:
        Encoder enc;
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
            enc.beginDict();
                enc.writeKey(C4STR(kC4ReplicatorAuthType));
                enc.writeString("Basic"_sl);
                enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
                enc.writeString(user);
                enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
                enc.writeString(password);
            enc.endDict();
        enc.endDict();
        _options = AllocedDict(enc.finish());
        
        std::array<C4Collection*, N> ret;
        for (size_t i = 0; i < N; ++i) {
            if (kC4DefaultCollectionSpec != collections[i]) {
                db->createCollection(collections[i]);
            }
            ret[i] = db->getCollection(collections[i]);
        }

        if (logLevel != kC4LogNone) {
            c4log_setLevel(kC4SyncLog, logLevel);
        }
        // This would effectively avoid flushing the bucket before the test.
        _flushedScratch = true;
        return ret;
    }

    template<size_t N>
    // propertyEncryption: 0, no encryption; 1, encryption only; 2, encryption and decryption
    void verifyDocs(const std::array<C4CollectionSpec, N>& collectionSpecs,
                    const std::array<unordered_map<alloc_slice, unsigned>, N>& docIDs,
                    bool checkRev =false, int propertyEncryption =0) {
        resetVerifyDb();
        std::array<C4Collection*, N> collections;
        for (size_t i = 0; i < N; ++i) {
            if (collectionSpecs[i] != Default) {
                verifyDb->createCollection(collectionSpecs[i]);
            }
            collections[i] = verifyDb->getCollection(collectionSpecs[i]);
            CHECK(0 == c4coll_getDocumentCount(collections[i]));
        }

        // Pull to verify that Push successfully pushed all documents in docIDs

        std::array<C4ReplicationCollection, N> replCollections;
        for (size_t i = 0; i < N; ++i) {
            replCollections[i] =
            C4ReplicationCollection{collectionSpecs[i], kC4Disabled, kC4OneShot};
        }
        std::array<AllocedDict, N> docIDsDicts;
        C4ParamsSetter paramsSetter = [&](C4ReplicatorParameters& c4Params) {
            c4Params.collectionCount = replCollections.size();
            c4Params.collections = replCollections.data();
            for (size_t i = 0; i < N; ++i) {
                fleece::Encoder enc;
                enc.beginArray();
                for (const auto& d : docIDs[i]) {
                    enc.writeString(d.first);
                }
                enc.endArray();
                Doc doc {enc.finish()};
                docIDsDicts[i] =
                    repl::Options::updateProperties(
                        AllocedDict(c4Params.collections[i].optionsDictFleece),
                        kC4ReplicatorOptionDocIDs,
                        doc.root());
                c4Params.collections[i].optionsDictFleece = docIDsDicts[i].data();
            }
            if (propertyEncryption > 0) {
                c4Params.propertyEncryptor = (C4ReplicatorPropertyEncryptionCallback)propEncryptor;
                c4Params.propertyDecryptor = (C4ReplicatorPropertyDecryptionCallback)propDecryptor;
            }
        };
        if (propertyEncryption == 1) {
            _options = repl::Options::updateProperties(_options,
                                                       kC4ReplicatorOptionDisablePropertyDecryption,
                                                       true);
            std::for_each(decContextMap->begin(), decContextMap->end(), [=](auto& p) {
                p.second.collection = c4db_getCollection(verifyDb, p.first, ERROR_INFO());
            });
        }

        {
            C4Database* savedb = db;
            DEFER {
                db = savedb;
            };
            db = verifyDb;
            replicate(paramsSetter);
        }

        for (size_t i = 0; i < N; ++i) {
            if (checkRev) {
                unsigned count = 0;
                c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(collections[i],
                                                                     nullptr, ERROR_INFO());
                {
                    ++count;
                    while (c4enum_next(e, ERROR_INFO())) {
                        C4DocumentInfo info;
                        c4enum_getDocumentInfo(e, &info);
                        auto it = docIDs[i].find(info.docID);
                        CHECK(it != docIDs[i].end());
                        CHECK(it->second == c4rev_getGeneration(info.revID));
                    }
                }
                CHECK(count == docIDs.size());
            } else {
                auto count = c4coll_getDocumentCount(collections[i]);
                REQUIRE(count == docIDs[i].size());
            }
        }
    }

    // Returns unique prefix based on time.
    static string timePrefix() {
        auto now = std::chrono::high_resolution_clock::now();
        auto epoch = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
        std::stringstream ss;
        ss << std::hex << seconds << "-";
        return ss.str();
    }

#if 0
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
#endif // class ResolvedDocument
    
    struct CipherContext {
        C4Collection* collection;
        slice docID;
        slice keyPath;
        bool called;

        CipherContext(C4Collection* c, const char* id, const char* path, bool called_)
        : collection(c)
        , docID(id)
        , keyPath(path)
        , called(called_)
        {}
    };

    using CipherContextMap = unordered_map<C4CollectionSpec, CipherContext>;
    std::unique_ptr<CipherContextMap> encContextMap;
    std::unique_ptr<CipherContextMap> decContextMap;
};

// map: docID -> rev generation
static std::unordered_map<alloc_slice, unsigned> getDocIDs(C4Collection* collection) {
    std::unordered_map<alloc_slice, unsigned> ret;
    c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(collection, nullptr, ERROR_INFO());
    {
        while (c4enum_next(e, ERROR_INFO())) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            ret.emplace(info.docID, c4rev_getGeneration(info.revID));
        }
    }
    return ret;
}

// The collection does not exist in the remote.
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Use Nonexisting Collections SG", "[.SyncServerCollection]") {
    //    constexpr size_t collectionCount = 2;
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        // C4CollectionSpec{"dummy1"_sl, kC4DefaultScopeID},
          C4CollectionSpec{"dummy2"_sl, kC4DefaultScopeID} };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    string idPrefix = timePrefix();
    importJSONLines(sFixturesDir + "names_100.json", collections[0], 0, false, 2, idPrefix);
    
    std::array<C4ReplicationCollection, collectionCount> replCollections = {
        C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled},
    };
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    replicate(paramsSetter, false);
    // ERROR: {Repl#7} Got LiteCore error: WebSocket error 404, "Collection 'dummy2'
    // is not found on the remote server"
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 404);
    
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Single Collection SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    constexpr size_t docCount = 20;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection*, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    
    bool continuous = false;

    SECTION("Named Collection") {
        collectionSpecs = {Roses};
    }
    
    SECTION("Default Collection") {
        collectionSpecs = {Default};
        // Not ready:
        return;
    }

    SECTION("Named Collection Continuous") {
        collectionSpecs = {Roses};
        continuous = true;
    }

    collections = collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    importJSONLines(sFixturesDir + "names_100.json", collections[0], 0, false, docCount, idPrefix);
    docIDs[0] = getDocIDs(collections[0]);

    // collectionCount == 1;
    std::array<C4ReplicationCollection, collectionCount> replCollections = {
        C4ReplicationCollection{collectionSpecs[0], continuous ? kC4Continuous : kC4OneShot, kC4Disabled},
    };
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Multiple Collections SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 3;
    constexpr size_t docCount = 20;
    bool continuous = false;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection*, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docInfos;
    
    // Three collections:
    // 1. Guitars - in the default scope
    // 2. Roses   - in scope "flowers"
    // 3. Tulips  - in scope "flowers

    SECTION("1-2-3") {
        collectionSpecs = {Guitars, Roses, Tulips};
        // not ready
        return;
    }

    SECTION("3-2-1") {
        collectionSpecs = {Tulips, Roses, Guitars};
        // not ready
        return;
    }

    SECTION("2-1-3") {
        collectionSpecs = {Roses, Guitars, Tulips};
        continuous = true;
        (void)continuous;
        // not ready
        return;
    }

    collections = collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    for (int i = 0; i < collectionCount; ++i) {
        importJSONLines(sFixturesDir + "names_100.json", collections[i], 0, false, docCount, idPrefix);
        docInfos[i] = getDocIDs(collections[i]);
    }

    // Push:

    std::array<C4ReplicationCollection, collectionCount> replCollections;
    for (int i = 0; i < collectionCount; ++i) {
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i],
            continuous ? kC4Continuous : kC4OneShot,
            kC4Disabled};
    }
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docInfos);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Push SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
        //, Tulips
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 10, idPrefix);
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i],
            kC4OneShot, kC4Disabled};
    }
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 5, idPrefix);
        docIDs[i] = getDocIDs(collections[i]);
    }

    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Revisions SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
        //, Tulips
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 2, idPrefix + "db-" + string(collectionSpecs[i].name));
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4Continuous, kC4Disabled};
    }

    Jthread jthread;
    _callbackWhenIdle = [=, &jthread, &docIDs]() {
        jthread.thread = std::thread(std::thread{[=, &docIDs]() mutable {
            for (size_t i = 0; i < collectionCount; ++i) {
                string collName = string(collectionSpecs[i].name);
                string docID = idPrefix + "-" + collName + "-docko";
                ReplicatorLoopbackTest::addRevs(collections[i], 500ms, alloc_slice(docID), 1, 10, true, ("db-"s + collName).c_str());
                docIDs[i].emplace(alloc_slice(docID), 10); // rev 3
            }
            _stopWhenIdle.store(true);
        }});
        _callbackWhenIdle = nullptr;
    };

    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    replicate(paramsSetter);
    // total 3 docs, 12 revs.
    CHECK(_callbackStatus.progress.documentCount == 12);
    verifyDocs(collectionSpecs, docIDs, true);
}

#if 0
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
#endif

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push and Pull Attachments SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    //    constexpr size_t collectionCount = 2;
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
        //, Tulips
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    vector<string> attachments1 = {idPrefix+"Attachment A", idPrefix+"Attachment B", idPrefix+"Attachment Z"};
    std::array<vector<C4BlobKey>, collectionCount> blobKeys; // blobKeys1a, blobKeys1b;
    {
        string doc1 = idPrefix + "doc1";
        string doc2 = idPrefix + "doc2";
        TransactionHelper t(db);
        for (size_t i = 0; i < collectionCount; ++i) {
            blobKeys[i] = addDocWithAttachments(db, collectionSpecs[i],  slice(doc1), attachments1, "text/plain");
            docIDs[i] = getDocIDs(collections[i]);
            replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4Disabled};
        }
    }

    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);
    for (size_t i = 0; i < collectionCount; ++i) {
        checkAttachments(verifyDb, blobKeys[i], attachments1);
    }
}

#if 0
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
#endif

#ifdef COUCHBASE_ENTERPRISE

static void validateCipherInputs(ReplicatorCollectionSGTest::CipherContextMap* ctx,
                                 C4CollectionSpec& spec, C4String& docID, C4String& keyPath) {
    auto i = ctx->find(spec);
    REQUIRE(i != ctx->end());

    auto& context = i->second;
    CHECK(spec == context.collection->getSpec());
    CHECK(docID == context.docID);
    CHECK(keyPath == context.keyPath);
    context.called = true;
}

C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError)
{
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->encContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, 1));
}

C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm,
                                   C4String keyID, C4Error* outError)
{
    auto test = static_cast<ReplicatorCollectionSGTest*>(ctx);
    validateCipherInputs(test->decContextMap.get(), spec, docID, keyPath);
    return C4SliceResult(ReplicatorLoopbackTest::UnbreakableEncryption(input, -1));
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Replicate Encrypted Properties with Collections SG", "[.SyncServerCollection]") {
    const bool TestDecryption = GENERATE(false, true);
    C4Log("---- %s decryption ---", (TestDecryption ? "With" : "Without"));

    string idPrefix = timePrefix();
    //    constexpr size_t collectionCount = 2;
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
        //, Tulips
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password", kC4LogNone);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    encContextMap.reset(new CipherContextMap);
    decContextMap.reset(new CipherContextMap);
    string docs[] = {idPrefix + "hiddenRose", idPrefix + "invisibleTulip"};
    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    {
        TransactionHelper t(db);
        for (size_t i = 0; i < collectionCount; ++i) {
            createFleeceRev(collections[i], slice(docs[i]), kRevID, originalJSON);
            docIDs[i] = getDocIDs(collections[i]);
            replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4Disabled};
            encContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(collectionSpecs[i]),
                                   std::forward_as_tuple(collections[i], docs[i].c_str(), "xNum", false));
            decContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(collectionSpecs[i]),
                                   std::forward_as_tuple(collections[i], docs[i].c_str(), "xNum", false));
        }
    }

    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
        c4Params.propertyEncryptor = propEncryptor;
        c4Params.propertyDecryptor = propDecryptor;
    };

    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs, true, TestDecryption ? 2 : 1);

    // Check encryption on active replicator:
    for (auto i = encContextMap->begin(); i != encContextMap->end(); i++) {
        CipherContext& context = i->second;
        CHECK(context.called);
    }

    // Check decryption on verifyDb:
    for (auto i = decContextMap->begin(); i != decContextMap->end(); i++) {
        auto& context = i->second;
        c4::ref<C4Document> doc = c4coll_getDoc(context.collection, context.docID, true,
                                                kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);

        if (TestDecryption) {
            CHECK(context.called);
            CHECK(props.toJSON(false, true) == originalJSON);
        } else {
            CHECK(!context.called);
            CHECK(props.toJSON(false, true) == R"({"encrypted$xNum":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);

            // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
            // so the property won't be converted back from the server schema.
            slice cipherb64 = props["encrypted$xNum"].asDict()["ciphertext"].asString();
            auto cipher = base64::decode(cipherb64);
            alloc_slice clear = ReplicatorLoopbackTest::UnbreakableEncryption(cipher, -1);
            CHECK(clear == "\"123-45-6789\"");
        }
    }
}
#endif //#ifdef COUCHBASE_ENTERPRISE

#if 0
TEST_CASE_METHOD(ReplicatorCollectionTest, "Filters & docIDs with Multiple Collections", "[Sync][Filters]") {
    string db_roses = "db-roses-";
    string db_tulips = "db-tulips-";
    string db2_roses = "db2-roses-";
    string db2_tulips = "db2-tulips-";
    addDocs(db, Roses, 10, db_roses);
    addDocs(db, Tulips, 10, db_tulips);
    addDocs(db, Lavenders, 10);
    addDocs(db2, Roses, 20, db2_roses);
    addDocs(db2, Tulips, 20, db2_tulips);
    addDocs(db2, Lavenders, 20);

    SECTION("PUSH") {
        C4ReplicatorValidationFunction pushFilter
            = [](C4CollectionSpec collectionSpec,
                 C4String docID,
                 C4String revID,
                 C4RevisionFlags,
                 FLDict body,
                 void* context) {
                CHECK(collectionSpec == Roses);
                slice drop {(const char*)context };
                return drop != docID;
            };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                // Assign pushFilter to Roses
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Roses) {
                    o.pushFilter = pushFilter;
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

        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        c4::ref<C4Document> rose1
        = c4coll_getDoc(roses2, "db-roses-1"_sl, true, kDocGetMetadata, ERROR_INFO());
        c4::ref<C4Document> tulip1
        = c4coll_getDoc(tulips2, "db-tulips-1"_sl, true, kDocGetMetadata, ERROR_INFO());
        
        CHECK(!rose1);
        CHECK( tulip1);
    }

    SECTION("PULL") {
        C4ReplicatorValidationFunction pullFilter
            = [](C4CollectionSpec collectionSpec,
                 C4String docID,
                 C4String revID,
                 C4RevisionFlags,
                 FLDict body,
                 void* context) {
                CHECK(collectionSpec == Tulips);
                slice drop {(const char*)context };
                return drop != docID;
            };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                // Assign pullFilter to Tulips
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Tulips) {
                    o.pullFilter = pullFilter;
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

        C4Collection* roses2  = getCollection(db2, Roses);
        C4Collection* tulips2 = getCollection(db2, Tulips);
        c4::ref<C4Document> rose1
        = c4coll_getDoc(roses2, "db-roses-1"_sl, true, kDocGetMetadata, ERROR_INFO());
        c4::ref<C4Document> tulip1
        = c4coll_getDoc(tulips2, "db-tulips-1"_sl, true, kDocGetMetadata, ERROR_INFO());

        CHECK( rose1);
        CHECK(!tulip1);
    }

    SECTION("DocIDs on PULL") {
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString("db-tulips-2"_sl);
        enc.writeString("db-tulips-7"_sl);
        enc.writeString("db-tulips-4"_sl);
        enc.endArray();
        Doc docIDs {enc.finish()};
        _updateClientOptions = [&](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Tulips) {
                    o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root());
                }
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
        Doc docIDs {enc.finish()};
        C4ReplicatorValidationFunction pullFilter
            = [](C4CollectionSpec collectionSpec,
                 C4String docID,
                 C4String revID,
                 C4RevisionFlags,
                 FLDict body,
                 void* context) {
                // filters are applied after docIDs
                CHECK((docID == "db-tulips-2"_sl || docID == "db-tulips-4"_sl || docID == "db-tulips-7"_sl));
                return docID != "db-tulips-4"_sl;
            };
        _updateClientOptions = [&](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Tulips) {
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
        Doc docIDs {enc.finish()};

        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Roses) {
                    o.setProperty(slice(kC4ReplicatorOptionDocIDs), docIDs.root());
                }
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
        Doc docIDs {enc.finish()};

        C4ReplicatorValidationFunction pushFilter
            = [](C4CollectionSpec collectionSpec,
                 C4String docID,
                 C4String revID,
                 C4RevisionFlags,
                 FLDict body,
                 void* context) {
                CHECK((docID == "db-roses-2"_sl || docID == "db-roses-4"_sl || docID == "db-roses-7"_sl));
                return docID != "db-roses-4"_sl;
            };
        _updateClientOptions = [=](const repl::Options& opts) {
            repl::Options ret = opts;
            for (repl::Options::CollectionOptions& o : ret.collectionOpts) {
                if (repl::Options::collectionPathToSpec(o.collectionPath) == Roses) {
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
#endif
