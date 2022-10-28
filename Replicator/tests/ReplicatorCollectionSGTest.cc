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
#include "fleece/Fleece.h"
#include <array>
#include <fstream>
#include <iostream>

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
// Once the DB has been set up, you can run sg_setup.sh, or set up SG manually with the configs below.
// sg_setup.sh should be run with the bucket name as the argument (i.e. './sg_setup.sh couch').
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
    ReplicatorCollectionSGTest()
        : ReplicatorAPITest() {
        pinnedCert = C4Test::readFile("Replicator/tests/data/cert/cert.pem");
        _address = {kC4Replicator2TLSScheme,
                    C4STR("localhost"),
                    4984};
    }
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
        ss << std::hex << seconds << "_";
        return ss.str();
    }
    
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

    static alloc_slice addChannelToJSON(slice json, slice ckey, const vector<string>& channelIDs) {
        MutableDict dict {FLMutableDict_NewFromJSON(json, nullptr)};
        MutableArray arr = MutableArray::newArray();
        for (const auto& chID : channelIDs) {
            arr.append(chID);
        }
        dict.set(ckey, arr);
        return dict.toJSON();
    }

    bool assignUserChannel(const vector<string>& channelIDs, C4Error* err) {
        auto bodyWithChannel = addChannelToJSON("{}"_sl, "admin_channels"_sl, channelIDs);
        HTTPStatus status;
        alloc_slice saveAuthHeader = _authHeader;
        _authHeader = AdminAuthHeader;
        sendRemoteRequest("PUT", "_user/sguser", &status,
                                err == nullptr ? ERROR_INFO() : ERROR_INFO(err),
                                bodyWithChannel, true);
        _authHeader = saveAuthHeader;
        return status == HTTPStatus::OK;
    }

    bool createTestUser(const vector<string> channelIDs) {
        // Delete it first.
        HTTPStatus status;
        alloc_slice savedAuthHeader = _authHeader;
        _authHeader = AdminAuthHeader;
        sendRemoteRequest("DELETE", "_user/"s+TestUser, &status, ERROR_INFO(), nullslice, true);

        string body = "{\"name\":\""s + TestUser + "\",\"password\":\"password\"}";
        alloc_slice bodyWithChannel = addChannelToJSON(slice(body), "admin_channels"_sl, channelIDs);
        sendRemoteRequest("POST", "_user", &status, ERROR_INFO(), bodyWithChannel, true);
        _authHeader = savedAuthHeader;
        return status == HTTPStatus::Created;
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

    static constexpr const char* TestUser = "test_user";

    // base-64 encoded of "sguser:password"
    static constexpr slice SGUserAuthHeader = "Basic c2d1c2VyOnBhc3N3b3Jk"_sl;
    // base-64 of "test_user:password"
    static constexpr slice TestUserAuthHeader = "Basic dGVzdF91c2VyOnBhc3N3b3Jk"_sl;
    // base-64 of, "Administrator:password"
    static constexpr slice AdminAuthHeader = "Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA=="_sl;
};


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push 5000 Changes Collections SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    const string docID = idPrefix + "apipfcc-doc1";
    
    string revID;
    constexpr size_t collectionCount = 1;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    collectionSpecs = {
        Roses
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    replCollections = {
        C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled},
    };

    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    {
        TransactionHelper t(db);
        revID = createNewRev(collections[0], slice(docID), nullslice, kFleeceBody);
    }

    docIDs[0] = getDocIDs(collections[0]);
    
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);

    C4Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for (int i = 2; i <= 5000; ++i)
            revID = createNewRev(collections[0], slice(docID), slice(revID), kFleeceBody);
            REQUIRE(!revID.empty());
    }

    C4Log("-------- Second Replication --------");
    replicate(paramsSetter);
    verifyDocs(collectionSpecs, docIDs);
    
}

// The collection does not exist in the remote.
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Use Nonexisting Collections SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    //    constexpr size_t collectionCount = 2;
    constexpr size_t collectionCount = 1;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    collectionSpecs = {
        // C4CollectionSpec{"dummy1"_sl, kC4DefaultScopeID},
        C4CollectionSpec{"dummy2"_sl, kC4DefaultScopeID}
    };
    replCollections = {
        C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled},
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    
    importJSONLines(sFixturesDir + "names_100.json", collections[0], 0, false, 2, idPrefix);
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
    std::array<C4ReplicationCollection, collectionCount> replCollections;

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
    replCollections = {
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
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> localDocIDs;
    std::vector<AllocedDict> allocedDicts;

    collectionSpecs = {
        Roses
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
   
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    collectionSpecs = {
        Roses
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    collectionSpecs = {
        Roses
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    std::array<vector<C4BlobKey>, collectionCount> blobKeys; // blobKeys1a, blobKeys1b;

    collectionSpecs = {
        Roses
        //, Tulips
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    
    vector<string> attachments1 = {idPrefix+"Attachment A", idPrefix+"Attachment B", idPrefix+"Attachment Z"};
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;
    std::array<string, collectionCount> collNames;
    std::vector<AllocedDict> allocedDicts;

    collectionSpecs = {
        Roses
    };
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    collNames = {"rose"};

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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Revoke Access - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "-a";
    const string channelIDb = idPrefix + "-b";

    // Create a temporary user for this test
    HTTPStatus status;
    C4Error error;
    sendRemoteRequest("POST", "_user", &status, &error, R"({"name":"purgeRevoke","password":"password"})", true);
    sendRemoteRequest("PUT", "_user/purgeRevoke", &status, &error, R"({"admin_channels":["*"]})", true);
    REQUIRE(status == HTTPStatus::OK);

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // One-shot pull setup
    constexpr size_t collectionCount = 1;
    const std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses
    };
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionChannels));
    enc.beginArray();
    enc.writeString(channelIDa);
    enc.writeString(channelIDb);
    enc.endArray();
    enc.endDict();
    fleece::alloc_slice opts { enc.finish() };

    std::array<C4Collection*, collectionCount> collections =
            collectionPreamble(collectionSpecs, "purgeRevoke", "password");
    std::array<C4ReplicationCollection, collectionCount> replCollections {
        {{ // three sets of braces? because Xcode
            collectionSpecs[0], kC4Disabled, kC4OneShot,
            opts, nullptr, _pullFilter, this
        }}
    };
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    auto collRoses = collections[0];

    auto collRosesPath = repl::Options::collectionSpecToPath(collectionSpecs[0]);

    // Put doc in remote DB, in channels a and b
    sendRemoteRequest("PUT", collRosesPath, docIDstr, &status, &error,
                      addChannelToJSON("", "channels"_sl, { channelIDa, channelIDb }));
    REQUIRE(status == HTTPStatus::Created);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(paramsSetter);

    // Verify:
    c4::ref<C4Document> doc1 = c4coll_getDoc(collRoses, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoked access to channel 'a':
    sendRemoteRequest("PUT", "_user/purgeRevoke", &status, &error,
                      addChannelToJSON("", "admin_channels"_sl, { channelIDb }), true);
    REQUIRE(status == HTTPStatus::OK);

    // Check if update to doc1 is still pullable:
    auto oRevID = slice(doc1->revID).asString();
    sendRemoteRequest("PUT", collRosesPath, docIDstr, &status, &error,
                       addChannelToJSON(R"({"_rev":")" + oRevID, "channels"_sl, { channelIDb }));

    C4Log("-------- Pull update");
    replicate(paramsSetter);

    // Verify the update:
    doc1 = c4coll_getDoc(collRoses, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    auto curRevID = slice(doc1->revID).asString();

    // Revoke access to all channels:
    sendRemoteRequest("PUT", "_user/purgeRevoke", &status, &error, R"({"admin_channels":[]})", true);
    REQUIRE(status == HTTPStatus::OK);

    C4Log("-------- Pull the revoked");
    replicate(paramsSetter);

    // Verify that doc1 is purged:
    doc1 = c4coll_getDoc(collRoses, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(!doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);

    // Purge remote doc so test will succeed multiple times without flushing bucket
    sendRemoteRequest("POST", "_purge", &status, &error, "{\"" + docIDstr + "\":[\"*\"]}", true);
    // Delete temp user
    sendRemoteRequest("DELETE", "_user/purgeRevoke", &status, &error, nullslice, true);
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

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection *, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    collectionSpecs = {
        Roses};
    collections = collectionPreamble(collectionSpecs, "sguser", "password");
    
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

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pinned Certificate Success - SGColl", "[.SyncServerCollection]") {
    // Leaf cert (Replicator/tests/data/cert/sg_cert.pem (1st cert))
    pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIICqzCCAZMCFGrxed0RuxP+uYOzr9wIeRp4gBjHMA0GCSqGSIb3DQEBCwUAMBAx
DjAMBgNVBAMMBUludGVyMB4XDTIyMTAyNTEwMjAzMFoXDTMyMTAyMjEwMjAzMFow
FDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAknbSS/newbZxs4afkUEgMO9WzE1LJAZ7oj3ovLzbsDYVJ3Ct1eBA2yYN
t87ROTvJ85mw4lQ3puMhWGGddYUQzBT7rdtpvydk9aNIefLwU6Yn6YvXC1asxSsb
yFr75j21UZ+qHZ1B4DYAR09Qaps43OKGKJl+4QBUkcLp+Hgo+5e29buv3VvoSK42
MnYsFFtgjVsLBJcL0L9t5gxujPiK8jbdXDYN3Md602rKua9LNwff02w8FWJ8/nLZ
LxtAVidgHJPEY2kDj+S2fUOaAypHcvkHAJ9KKwqHYpwvWzv32WpmmpKBxoiP2NFI
655Efmx7g3pJ2LvUbyOthi8k/VT3/wIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQC3
c+kGcvn3d9QyGYif2CtyAYGRxUQpMjYjqQiwyZmKNp/xErgns5dD+Ri6kEOcq0Zl
MrsPV5iprAKCvEDU6CurGE+sUiJH1csjPx+uCcUlZwT+tZF71IBJtkgfQx2a9Wfs
CA+qS9xaNhuYFkbSIbA5uiSUf9MRxafY8mqjtrOtdPf4fxN5YVsbOzJLtrcVVL9i
Y5rPGtUwixeiZsuGXYkFGLCZx8DWQQrENSu3PI5hshdHgPoHyqxls4yDTDyF3nqq
w9Q3o9L/YDg9NGdW1XQoBgxgKy5G3YT7NGkZXUOJCHsupyoK4GGZQGxtb2eYMg/H
lTIN5f2LxWf+8kJqfjlj
-----END CERTIFICATE-----)");

    // Ensure TLS connection to SGW
    if(!Address::isSecure(_address)) {
        _address = {kC4Replicator2TLSScheme,
                    C4STR("localhost"),
                    4984};
    }
    REQUIRE(Address::isSecure(_address));

    // One-shot push setup
    constexpr size_t collectionCount = 1;
    const std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses
    };
    std::array<C4Collection*, collectionCount> collections =
            collectionPreamble(collectionSpecs, "sguser", "password");
    (void)collections;
    std::array<C4ReplicationCollection, collectionCount> replCollections {
            {{ // three sets of braces? because Xcode
                     collectionSpecs[0], kC4OneShot, kC4Disabled
             }}
    };
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    replicate(paramsSetter);

    // Intermediate cert (Replicator/tests/data/cert/sg_cert.pem (2nd cert))
    pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDRzCCAi+gAwIBAgIUQu1TjW0ZRWGCKRQh/JcZxfG/J/YwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAQMQ4wDAYDVQQDDAVJbnRlcjCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAL9WuYxf16AXrxJlSi/hL5cVDWozhfQ2Qb9c5zl3
zPLUmkDkgEq1Yma6pC46jFQsZE1Yqst6iXng/JX4R7azCNFFxyoorDMuynS52VgS
lfAUddIxi86DfM3rkzm/Yho+HoGCeDq+KIKyEQfZmKyVQj8LRQ/qzSAF11B4pp+e
zLD70XRfOZAwJC/utOHxruf+uTr7C3sW8wvW6MDaLsxc/eKptgamMtWe6kM1dkV3
IycEhHHTvrj0dWM7Bwko4OECZkoyzZWHOLNKetlkPQSq2zApHDOQdRin4iAbOGPz
hiJViXiI0pihOJM8yuHF6MuCB8u8JuAvY3c52+OCKQv4hLkCAwEAAaOBjDCBiTAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTLyGcuHP88QhUAmjCgBIwjZj/O2zBX
BgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4wHDEaMBgGA1UEAwwR
Q291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfxW5oFMA0GCSqGSIb3
DQEBCwUAA4IBAQCPDS2j9gQPzXYRNKL9wNiEO5CnrSf2X5b69OoznQRs0R37xUYo
LqFP4/4XFhtNSD6fHhA/pOYC3dIsKNl8+/5Pb4SROsnT6grjbf46bhbVlocKCm0f
gD2TG2OY64eMIpgaSw/WeFQxHmpqm9967iIOg30EqA4zH/hpCHCldFsqhu7FxJ0o
qp/Ps+yRh2PBGVbqkXAabtCnC4yPn1denqCdUPW2/yK7MzDEapMwkwdWVzzaWUy/
LJ46AUTOMWgFdr1+JcCxFKtIXHmL+nSkIlstEkA0jgYOUGSkKB2BxxtrEmnXFTsK
lb78xSgdpAaELOl18IEF5N3FHjVCtvXqStyS
-----END CERTIFICATE-----)");

    replicate(paramsSetter);

    // Root cert (Replicator/tests/data/cert/sg_cert.pem (3rd cert))
    pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDUzCCAjugAwIBAgIUYyeh7cxGMUNIwkVN1PGCx/FbmgUwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAcMRowGAYDVQQDDBFDb3VjaGJhc2UgUm9vdCBDQTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMM9/S7xfgMZF+J4iBxnJEai
cW/FpPsM9HJUt4Xs+JNb+1nJOSo4eGYrAGk/wjxi+VcTdOb/8lrOmT4khKv9CExb
WdxMdSqGb0TM2phd7ZPqCqoMVA0jGJ8ZxLaYlqPsyL9eRio4gVnSE5uNQjWyBEcB
z6eOn1rDZPvJlCF6fRcvgPhFVeIH7xb4jh1OzOoXgM1rrYPLAYr0vLEbk07TwFTE
fCMdBgjEiSnbzQrlgNoVTpcQrGjTmKrN52GC39eTW4tyLdxo+ipgqjiKeTO/qJBp
YZ8V7RgMjhyynIBxhxzZdDEXw5hWZV11kxA3dmBqup9aZ/cK3q2Cxe2mdgMv7aMC
AwEAAaOBjDCBiTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBQQSW+6ctHLjFGg
ZaWLvK61p616HDBXBgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4w
HDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfx
W5oFMA0GCSqGSIb3DQEBCwUAA4IBAQCD+qLQqDkjjVuMDRpvehWr46kKHOHVtXxH
FKpiDDYlD7NUqDWj4y1KKFHZuVg/H+IIflE55jv4ttqmKEMuEpUCd5SS3f9mTM0A
TqwzDVs9HfbuKb6lHtnJLTUvM9wBe/WPW8TCB50AkyMpz5sAAqpK4022Vein2C3T
0uox22kUBslWKZnXMtNeT3h2lFXcCZlQPLRfvHdtXA0t5We2kU0SPiFJc4I0OGjv
zzcNjA18pjiTtpuVeNBUAsBJcbHkNQLKnHGPsBNMAedVCe+AM5CVyZdDlZs//fov
0proEf3d58AqTx4i8uUZHdvmE3MVqeL2rrXFNB74Rs6j8QI1wlpW
-----END CERTIFICATE-----)");

    replicate(paramsSetter);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pinned Certificate Failure - SGColl", "[.SyncServerCollection]") {
    if(!Address::isSecure(_address)) {
        _address = { kC4Replicator2TLSScheme,
                    C4STR("localhost"),
                    4984 };
    }
    REQUIRE(Address::isSecure(_address));

    // Using an unmatched pinned cert:
    pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n" \
        "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n" \
        "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n" \
        "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n" \
        "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n" \
        "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n" \
        "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n" \
        "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n" \
        "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n" \
        "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n" \
        "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n" \
        "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n" \
        "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n" \
        "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n" \
        "S5TnM2/TjtQ=\r\n"                                                     \
        "-----END CERTIFICATE-----\r\n";

    // One-shot push setup
    constexpr size_t collectionCount = 1;
    const std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses
    };
    std::array<C4Collection*, collectionCount> collections =
            collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<C4ReplicationCollection, collectionCount> replCollections {
            {{ // three sets of braces? because Xcode
                     collectionSpecs[0], kC4OneShot, kC4Disabled
             }}
    };
    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    // expectSuccess = false so we can check the error code
    replicate(paramsSetter, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif //#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Remove Doc From Channel SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    // one collection now now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    string        doc1ID {idPrefix + "doc1"};
    vector<string> chIDs {idPrefix+"a", idPrefix+"b"};

    C4Error error;
    DEFER {
        // Don't REQUIRE. It would terminate the entire test run.
        assignUserChannel({"*"}, &error);
    };
    REQUIRE(assignUserChannel(chIDs, &error));

    // Create docs on SG:
    _authHeader = SGUserAuthHeader;
    for (size_t i = 0; i < collectionCount; ++i) {
        sendRemoteRequest("PUT", repl::Options::collectionSpecToPath(collectionSpecs[i]),
                          doc1ID, addChannelToJSON("{}"_sl, "channels"_sl, chIDs));
    }

    struct CBContext {
        int docsEndedTotal = 0;
        int docsEndedPurge = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;
        void reset() {
            docsEndedTotal = 0;
            docsEndedPurge = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } context;


    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void*) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            CBContext* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict flbody, void *context)
    {
        CBContext* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    for (size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = C4ReplicationCollection{
            collectionSpecs[i],
            kC4Disabled,
            kC4OneShot,
            {}, // properties
            nullptr, // pushFilter
            pullFilter,
            &context     // callbackContext
        };
    }

    bool autoPurgeEnabled {true};
    C4ParamsSetter paramsSetter {nullptr};
    SECTION("Auto Purge Enabled") {
        paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
            c4Params.collectionCount = replCollections.size();
            c4Params.collections     = replCollections.data();
        };
        autoPurgeEnabled = true;
    }

    std::vector<AllocedDict> allocedDicts;
    SECTION("Auto Purge Disabled") {
        paramsSetter = [&replCollections, &allocedDicts](C4ReplicatorParameters& c4Params) {
            c4Params.collectionCount = replCollections.size();
            c4Params.collections     = replCollections.data();
            allocedDicts.emplace_back(
                repl::Options::updateProperties(
                    AllocedDict(c4Params.optionsDictFleece),
                    C4STR(kC4ReplicatorOptionAutoPurge),
                    false)
                );
            c4Params.optionsDictFleece = allocedDicts.back().data();
        };
        autoPurgeEnabled = false;
    }

    replicate(paramsSetter);

    // Verify: (on collections[0] only
    c4::ref<C4Document> doc1 = c4coll_getDoc(collections[0], slice(doc1ID),
                                             true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 1);
    CHECK(context.docsEndedTotal == 1);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == 1);
    CHECK(context.pullFilterPurge == 0);

    // Removed doc from channel 'a':
    auto oRevID = slice(doc1->revID).asString();
    sendRemoteRequest("PUT", repl::Options::collectionSpecToPath(collectionSpecs[0]),
                      doc1ID, addChannelToJSON("{\"_rev\":\"" + oRevID + "\"}",
                                               "channels"_sl, {chIDs[1]}));

    C4Log("-------- Pull update");
    context.reset();
    replicate(paramsSetter);

    // Verify the update:
    doc1 = c4coll_getDoc(collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 2);
    CHECK(context.docsEndedTotal == 1);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == 1);
    CHECK(context.pullFilterPurge == 0);

    // Remove doc from all channels:
    oRevID = slice(doc1->revID).asString();
    sendRemoteRequest("PUT", repl::Options::collectionSpecToPath(collectionSpecs[0]),
                      doc1ID, addChannelToJSON("{\"_rev\":\"" + oRevID + "\"}",
                                               "channels"_sl, {}));

    C4Log("-------- Pull the removed");
    context.reset();
    replicate(paramsSetter);

    doc1 = c4coll_getDoc(collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    CHECK(context.docsEndedPurge == 1);
    if (autoPurgeEnabled) {
        // Verify if doc1 is purged:
        REQUIRE(!doc1);
        CHECK(context.pullFilterPurge == 1);
    } else {
        REQUIRE(doc1);
        // No pull filter called
        CHECK(context.pullFilterTotal == 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Removed Revision SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    // one collection for now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 1;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses
    };
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, TestUser, "password");
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    string doc1ID = idPrefix + "doc1";
    vector<string> chIDs {idPrefix+"a"};

    REQUIRE(createTestUser(chIDs));

    // Create docs on SG:
    _authHeader = SGUserAuthHeader;
    for (size_t i = 0; i < collectionCount; ++i) {
        sendRemoteRequest("PUT", repl::Options::collectionSpecToPath(collectionSpecs[i]),
                        doc1ID, addChannelToJSON("{}"_sl, "channels"_sl, chIDs));
    }

    struct CBContext {
         int docsEndedTotal = 0;
         int docsEndedPurge = 0;
         int pullFilterTotal = 0;
         int pullFilterPurge = 0;
         void reset() {
             docsEndedTotal = 0;
             docsEndedPurge = 0;
             pullFilterTotal = 0;
             pullFilterPurge = 0;
         }
     } cbContext;

    // Setup pull filter to filter the _removed rev:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict flbody, void *context)
    {
        CBContext* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void*) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            CBContext* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ctx->docsEndedPurge++;
            }
        }
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    for (size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = C4ReplicationCollection{
            collectionSpecs[i],
            kC4Disabled,
            kC4OneShot,
            {}, // properties
            nullptr, // pushFilter
            pullFilter,
            &cbContext     // callbackContext
        };
    }
    C4ParamsSetter paramsSetter
        = [&replCollections](C4ReplicatorParameters& c4Params)
    {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections     = replCollections.data();
    };
    replicate(paramsSetter);

    // Verify:
    c4::ref<C4Document> doc1 = c4coll_getDoc(collections[0], slice(doc1ID),
                                             true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(cbContext.docsEndedTotal == 1);
    CHECK(cbContext.docsEndedPurge == 0);
    CHECK(cbContext.pullFilterTotal == 1);
    CHECK(cbContext.pullFilterPurge == 0);
    
    // Remove doc from all channels
    auto oRevID = slice(doc1->revID).asString();
    for (size_t i = 0; i < collectionCount; ++i) {
        sendRemoteRequest("PUT", repl::Options::collectionSpecToPath(collectionSpecs[i]),
                          doc1ID, addChannelToJSON("{\"_rev\":\"" + oRevID + "\"}", "channels"_sl, {}));
    }

    C4Log("-------- Pull the removed");
    cbContext.reset();
    replicate(paramsSetter);

    // Verify if doc1 is not purged as the removed rev is filtered:
    doc1 = c4coll_getDoc(collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(cbContext.docsEndedPurge == 1);
    CHECK(cbContext.pullFilterPurge == 1);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled(default) - Delete Doc SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, TestUser, "password");
    std::array<C4ReplicationCollection, collectionCount> replCollections {
        { {collectionSpecs[0], kC4OneShot, kC4Disabled} }
    };

    string docID = idPrefix + "doc";
    vector<string> chIDs {idPrefix+"a"};

    REQUIRE(createTestUser(chIDs));

    // Create a doc and push it:
    c4::ref<C4Document> docs[collectionCount];
    {
        TransactionHelper t(db);
        C4Error error;
        for (size_t i = 0; i < collectionCount; ++i) {
            docs[i] = c4coll_createDoc(collections[i], slice(docID),
                                       json2fleece(addChannelToJSON("{}"_sl, "channels"_sl, chIDs).asString().c_str()),
                                       0, ERROR_INFO(error));
            REQUIRE(error.code == 0);
            REQUIRE(docs[i]);
        }
    }
    for (auto coll : collections) {
        REQUIRE(c4coll_getDocumentCount(coll) == 1);
    }

    C4ParamsSetter paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };
    replicate(paramsSetter);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error error;
        for (auto doc : docs) {
            doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
            REQUIRE(error.code == 0);
            REQUIRE(doc);
            REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
        }
    }
    for (auto coll : collections) {
        CHECK(c4coll_getDocumentCount(coll) == 0);
    }
    replicate(paramsSetter);

    // Apply a pull and verify that the document is not purged.
    for (size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4Disabled, kC4OneShot};
    }
    paramsSetter = [&replCollections](C4ReplicatorParameters& c4Params) {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections = replCollections.data();
    };

    replicate(paramsSetter);
    for (auto coll: collections) {
        C4Error error;
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID), true, kDocGetAll, ERROR_INFO(error));
        CHECK(error.code == 0);
        CHECK(doc != nullptr);
        REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
        CHECK(c4coll_getDocumentCount(coll) == 0);
    }
}
