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
 --data-raw '{"num_index_replicas": 0, "bucket": "your_bucket_name", "scopes": {"flowers": {"collections":{"roses":{}}}}}'
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
static constexpr slice LavenderName = "lavenders"_sl;
static constexpr slice FlowersScopeName = "flowers"_sl;

static constexpr C4CollectionSpec Roses = { RosesName, FlowersScopeName };
static constexpr C4CollectionSpec Tulips = { TulipsName, FlowersScopeName };
static constexpr C4CollectionSpec Lavenders = { LavenderName, FlowersScopeName };
static constexpr C4CollectionSpec Default = kC4DefaultCollectionSpec;

using namespace std;
using namespace litecore::repl;

#ifdef COUCHBASE_ENTERPRISE
static C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError);

static C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm,
                                   C4String keyID, C4Error* outError);
#endif

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
                            const char* user, const char* password) {
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

        // This would effectively avoid flushing the bucket before the test.
        _flushedScratch = true;
        return ret;
    }

    template<size_t N>
    static void setDocIDs(C4ReplicatorParameters& c4Params,
                   std::array<C4ReplicationCollection, N>& replCollections,
                   const std::array<unordered_map<alloc_slice, unsigned>, N>& docIDs,
                   std::vector<AllocedDict>& allocedDicts) {
        for (size_t i = 0; i < N; ++i) {
            fleece::Encoder enc;
            enc.beginArray();
            for (const auto& d : docIDs[i]) {
                enc.writeString(d.first);
            }
            enc.endArray();
            Doc doc {enc.finish()};
            allocedDicts.emplace_back(
                repl::Options::updateProperties(
                    AllocedDict(c4Params.collections[i].optionsDictFleece),
                    kC4ReplicatorOptionDocIDs,
                    doc.root())
            );
            c4Params.collections[i].optionsDictFleece = allocedDicts.back().data();
        }
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
        std::vector<AllocedDict> allocedDicts;
        C4ParamsSetter paramsSetter = [&](C4ReplicatorParameters& c4Params) {
            c4Params.collectionCount = replCollections.size();
            c4Params.collections = replCollections.data();
            setDocIDs(c4Params, replCollections, docIDs, allocedDicts);
#ifdef COUCHBASE_ENTERPRISE
            if (propertyEncryption > 0) {
                c4Params.propertyEncryptor = (C4ReplicatorPropertyEncryptionCallback)propEncryptor;
                c4Params.propertyDecryptor = (C4ReplicatorPropertyDecryptionCallback)propDecryptor;
            }
#endif
        };
#ifdef COUCHBASE_ENTERPRISE
        if (propertyEncryption == 1) {
            _options = repl::Options::updateProperties(_options,
                                                       kC4ReplicatorOptionDisablePropertyDecryption,
                                                       true);
            std::for_each(decContextMap->begin(), decContextMap->end(), [=](auto& p) {
                p.second.collection = c4db_getCollection(verifyDb, p.first, ERROR_INFO());
            });
        }
#else
        (void)propertyEncryption;
#endif
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
        collectionPreamble(collectionSpecs, "sguser", "password");
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
    
    SECTION("Another Named Collection") {
        collectionSpecs = {Lavenders};
        // Not ready:
        return;
    }

    SECTION("Named Collection Continuous") {
        collectionSpecs = {Roses};
        continuous = true;
    }

    collections = collectionPreamble(collectionSpecs, "sguser", "password");
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

    collections = collectionPreamble(collectionSpecs, "sguser", "password");
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Push & Pull SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 20, idPrefix+"remote-");
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4Disabled};
    }

    // Send the docs to remote
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);

    deleteAndRecreateDB();

    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> localDocIDs;
    for (size_t i = 0; i < collectionCount; ++i) {
        collections[i] = c4db_createCollection(db, collectionSpecs[i], ERROR_INFO());
        addDocs(collections[i], 10, idPrefix+"local-");
        localDocIDs[i] = getDocIDs(collections[i]);
        // OneShot Push & Pull
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4OneShot};
    }
    
    // Merge together the doc IDs
    for (size_t i = 0; i < collectionCount; ++i) {
        for (auto iter = localDocIDs[i].begin(); iter != localDocIDs[i].end(); ++iter) {
            docIDs[i].emplace(iter->first, iter->second);
        }
    }

    std::vector<AllocedDict> allocedDicts;
    paramsSetter = [&replCollections, &docIDs, &allocedDicts](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
        setDocIDs(c4Params, replCollections, docIDs, allocedDicts);
    };

    replicate(paramsSetter);
    // 10 docs are pushed and 20 docs are pulled from each collectiion.
    CHECK(_callbackStatus.progress.documentCount == 30*collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Push SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    // one collection now now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 1;
    
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Revisions SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    // one collection now now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push and Pull Attachments SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    //    constexpr size_t collectionCount = 2;
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
        //, Tulips
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Resolve Conflict SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    std::array<string, collectionCount> collNames = {"rose"};

    for (size_t i = 0; i < collectionCount; ++i) {
        createFleeceRev(collections[i], slice(idPrefix+collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(collections[i], slice(idPrefix+collNames[i]), revOrVersID("2-12121212", "1@cafe"),
                        "{\"db\":\"remote\"}"_sl);
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4Disabled};
    }

    // Send the docs to remote
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs, true);
    
    deleteAndRecreateDB();
    for (size_t i = 0; i < collectionCount; ++i) {
        collections[i] = c4db_createCollection(db, collectionSpecs[i], ERROR_INFO());
        createFleeceRev(collections[i], slice(idPrefix+collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(collections[i], slice(idPrefix+collNames[i]), revOrVersID("2-13131313", "1@babe"),
                        "{\"db\":\"local\"}"_sl);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4Disabled, kC4OneShot};
    }

    std::vector<AllocedDict> allocedDicts;
    paramsSetter = [&replCollections, &docIDs, &allocedDicts](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
        setDocIDs(c4Params, replCollections, docIDs, allocedDicts);
    };
    _conflictHandler = [&](const C4DocumentEnded* docEndedWithConflict) {
        C4Error error;
        int i = -1;
        for (int k = 0; k < collectionCount; ++k) {
            if (docEndedWithConflict->collectionSpec == collectionSpecs[k]) {
                i = k;
            }
        }
        Assert(i >= 0, "Internal logical error");

        TransactionHelper t(db);

        slice docID = docEndedWithConflict->docID;
        // Get the local doc. It is the current revision
        c4::ref<C4Document> localDoc = c4coll_getDoc(collections[i], docID, true, kDocGetAll, WITH_ERROR(error));
        CHECK(error.code == 0);

        // Get the remote doc. It is the next leaf revision of the current revision.
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(collections[i], docID, true, kDocGetAll, &error);
        bool succ = c4doc_selectNextLeafRevision(remoteDoc, true, false, &error);
        Assert(remoteDoc->selectedRev.revID == docEndedWithConflict->revID);
        CHECK(error.code == 0);
        CHECK(succ);

        C4Document* resolvedDoc = nullptr;
        switch (i) {
            case 0:
                resolvedDoc = remoteDoc;
                break;
            default:
                Assert(false, "Unknown collection");
        }
        FLDict mergedBody = c4doc_getProperties(resolvedDoc);
        C4RevisionFlags mergedFlags = resolvedDoc->selectedRev.flags;
        alloc_slice winRevID = resolvedDoc->selectedRev.revID;
        alloc_slice lostRevID = (resolvedDoc == remoteDoc) ? localDoc->selectedRev.revID
                                                           : remoteDoc->selectedRev.revID;
        bool result = c4doc_resolveConflict2(localDoc, winRevID, lostRevID,
                                             mergedBody, mergedFlags, &error);
        Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s' in '%.*s.%.*s'",
               SPLAT(docID), SPLAT(collectionSpecs[i].scope), SPLAT(collectionSpecs[i].name));
        Assert((localDoc->flags & kDocConflicted) == 0);

        if (!c4doc_save(localDoc, 0, &error)) {
            Assert(false, "conflictHandler: c4doc_save failed for '%.*s' in '%.*s.%.*s'",
                   SPLAT(docID), SPLAT(collectionSpecs[i].scope), SPLAT(collectionSpecs[i].name));
        }
    };
    replicate(paramsSetter);

    for (int i = 0; i < collectionCount; ++i) {
        switch (i) {
            case 0: {
                c4::ref<C4Document> doc = c4coll_getDoc(collections[i], slice(idPrefix+collNames[i]),
                                                        true, kDocGetAll, nullptr);
                REQUIRE(doc);
                // Remote wins for the first collection
                CHECK(fleece2json(c4doc_getRevisionBody(doc)) == "{db:\"remote\"}"); // Remote Wins
                REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
            } break;
            default:
                Assert(false, "Not ready yet");
        }
    }
}


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
    // one collection now now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
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

