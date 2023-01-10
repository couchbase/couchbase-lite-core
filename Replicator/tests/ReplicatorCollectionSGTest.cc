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
#include "SGTestUser.hh"
#include "ReplParams.hh"
#include <array>
#include <iostream>
#include <typeinfo>

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
 curl -k --location --request PUT "https://localhost:4985/scratch/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw "{\"num_index_replicas\": 0, \"bucket\": \"$1\", \"scopes\": {\"flowers\": {\"collections\":{\"roses\":{}, \"tulips\":{}, \"lavenders\":{}}}}}"
 */
//  config SG user:
/*
 curl -k --location --request POST "https://localhost:4985/scratch/_user/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"name": "sguser", "password": "password", "collection_access": {"flowers": {"roses": {"admin_channels": ["*"]}, "tulips": {"admin_channels": ["*"]}, "lavenders": {"admin_channels": ["*"]}}}}'
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

static constexpr const char* kTestUserName = "test_user";

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
        _sg.pinnedCert = C4Test::readFile("Replicator/tests/data/cert/cert.pem");
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984};
        _sg.assignUserChannel("sguser", { Roses, Tulips, Lavenders }, {"*"});
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

    // An overload which allows simply passing an SG::TestUser object
    template<size_t N>
    std::array<C4Collection*, N>
    collectionPreamble(std::array<C4CollectionSpec, N> collections,
                       const SG::TestUser& testUser) {
        return collectionPreamble(collections, testUser._username.c_str(), testUser._password.c_str());
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

        std::vector<C4ReplicationCollection> replCollections {N};
        for (size_t i = 0; i < N; ++i) {
            replCollections[i] =
            C4ReplicationCollection{collectionSpecs[i], kC4Disabled, kC4OneShot};
        }
        ReplParams replParams { replCollections };
        replParams.setDocIDs(docIDs);
#ifdef COUCHBASE_ENTERPRISE
        if(propertyEncryption > 0) {
//            replParams.propertyEncryptor = (C4ReplicatorPropertyEncryptionCallback)propEncryptor;
            replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);
//            replParams.propertyDecryptor = (C4ReplicatorPropertyDecryptionCallback)propDecryptor;
        }
        if (propertyEncryption == 1) {
            replParams.setOption(kC4ReplicatorOptionDisablePropertyDecryption, true);
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
            replicate(replParams);
        }

        for (size_t i = 0; i < N; ++i) {
            if (checkRev) {
                c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(collections[i],
                                                                     nullptr, ERROR_INFO());
                unsigned count = 0;
                while (c4enum_next(e, ERROR_INFO())) {
                    ++count;
                    C4DocumentInfo info;
                    c4enum_getDocumentInfo(e, &info);
                    auto it = docIDs[i].find(info.docID);
                    CHECK(it != docIDs[i].end());
                    CHECK(it->second == c4rev_getGeneration(info.revID));
                }
                CHECK(count == docIDs[i].size());
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


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push 5000 Changes Collections SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    const string docID = idPrefix + "apipfcc-doc1";

    constexpr size_t collectionCount = 3;
    constexpr unsigned revisionCount = 2000;
    std::array<string, collectionCount> revIDs;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Tulips,
        Lavenders
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections {
        C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled},
        C4ReplicationCollection{collectionSpecs[1], kC4OneShot, kC4Disabled},
        C4ReplicationCollection{collectionSpecs[2], kC4OneShot, kC4Disabled},
    };

    ReplParams replParams { replCollections };

    {
        auto revID = revIDs.begin();
        TransactionHelper t(db);
        for (C4Collection * coll : collections) {
            *revID = createNewRev(coll, slice(docID), nullslice, kFleeceBody);
            REQUIRE(!(revID++)->empty());
        }
    }

    replicate(replParams);
    for (size_t i = 0; i < collectionCount; ++i) {
        docIDs[i] = getDocIDs(collections[i]);
    }
    verifyDocs(collectionSpecs, docIDs);

    C4Log("-------- Mutations --------");
    {
        auto revID = revIDs.begin();
        TransactionHelper t(db);
        for (auto coll: collections) {
            for (int i = 2; i <= revisionCount; ++i) {
                *revID = createNewRev(coll, slice(docID), slice(*revID), kFleeceBody);
                REQUIRE(!revID->empty());
            }
            ++revID;
        }
    }

    C4Log("-------- Second Replication --------");
    replicate(replParams);
    for (size_t i = 0; i < collectionCount; ++i) {
        docIDs[i] = getDocIDs(collections[i]);
    }
    verifyDocs(collectionSpecs, docIDs, true);
}

// The collection does not exist in the remote.
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Use Nonexisting Collections SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 4;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Tulips,
        C4CollectionSpec{"dummy1"_sl, kC4DefaultScopeID},
        Lavenders
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, "sguser", "password");

    std::vector<C4ReplicationCollection> replCollections(collectionCount);
    C4Error expectedError;

    SECTION("Collection does not exist at remote") {
        replCollections[0] =
            C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled};
        replCollections[1] =
            C4ReplicationCollection{collectionSpecs[1], kC4Disabled, kC4OneShot};
        replCollections[2] =
            C4ReplicationCollection{collectionSpecs[2], kC4OneShot, kC4OneShot};
        replCollections[3] =
            C4ReplicationCollection{collectionSpecs[3], kC4OneShot, kC4Disabled};
        // ERROR: {Repl#7} Got LiteCore error: WebSocket error 404, "Collection 'dummy2'
        // is not found on the remote server"
        expectedError = {WebSocketDomain, 404};
    }

    SECTION("Inconsistent Config") {
        replCollections[0] =
            C4ReplicationCollection{collectionSpecs[0], kC4OneShot, kC4Disabled};
        replCollections[1] =
            C4ReplicationCollection{collectionSpecs[1], kC4Disabled, kC4OneShot};
        replCollections[2] =
            C4ReplicationCollection{collectionSpecs[2], kC4OneShot, kC4OneShot};
        replCollections[3] =
            C4ReplicationCollection{collectionSpecs[3], kC4Continuous, kC4Disabled};
        expectedError = {LiteCoreDomain, kC4ErrorInvalidParameter};
    }

    for (auto coll : collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix);
    }

    ReplParams replParams { replCollections };
    replicate(replParams, false);

    CHECK(_callbackStatus.error.domain == expectedError.domain);
    CHECK(_callbackStatus.error.code == expectedError.code);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Sync with Single Collection SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 1;
    constexpr size_t docCount = 20;

    std::array<C4CollectionSpec, collectionCount> collectionSpecs;
    std::array<C4Collection*, collectionCount> collections;
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    bool continuous = false;

    SECTION("Named Collection") {
        collectionSpecs = {Roses};
    }

    SECTION("Default Collection") {
        collectionSpecs = {Default};
    }

    SECTION("Another Named Collection") {
        collectionSpecs = {Lavenders};
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
    ReplParams replParams { replCollections };

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    replicate(replParams);
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
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    // Three collections:
    // 1. Guitars - in the default scope. We currently cannot use collections from different scopes
    // 2. Roses   - in scope "flowers"
    // 3. Tulips  - in scope "flowers
    (void) Guitars;

    SECTION("1-2-3") {
        collectionSpecs = {Lavenders, Roses, Tulips};
    }

    SECTION("3-2-1") {
        collectionSpecs = {Tulips, Roses, Lavenders};
    }

    SECTION("2-1-3") {
        collectionSpecs = {Roses, Lavenders, Tulips};
        continuous = true;
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
    ReplParams replParams { replCollections };

    if (continuous) {
        _stopWhenIdle.store(true);
    }
    replicate(replParams);
    verifyDocs(collectionSpecs, docInfos);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Push & Pull SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Lavenders,
        Roses,
        Tulips
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections { collectionCount };
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> localDocIDs;

    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 20, idPrefix+"remote-");
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i], kC4OneShot, kC4Disabled};
    }

    // Send the docs to remote
    ReplParams replParams { replCollections };
    replicate(replParams);
    verifyDocs(collectionSpecs, docIDs);

    deleteAndRecreateDB();

    for (size_t i = 0; i < collectionCount; ++i) {
        collections[i] = c4db_createCollection(db, collectionSpecs[i], ERROR_INFO());
        addDocs(collections[i], 10, idPrefix+"local-");
        localDocIDs[i] = getDocIDs(collections[i]);
    }

    replParams.setPushPull(kC4OneShot, kC4OneShot);

    // Merge together the doc IDs
    for (size_t i = 0; i < collectionCount; ++i) {
        for (auto iter = localDocIDs[i].begin(); iter != localDocIDs[i].end(); ++iter) {
            docIDs[i].emplace(iter->first, iter->second);
        }
    }

    replParams.setDocIDs(docIDs);

    replicate(replParams);
    // 10 docs are pushed and 20 docs are pulled from each collectiion.
    CHECK(_callbackStatus.progress.documentCount == 30*collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Push SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Lavenders,
        Roses,
        Tulips
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 10, idPrefix);
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = C4ReplicationCollection{collectionSpecs[i],
            kC4OneShot, kC4Disabled};
    }
    ReplParams replParams { replCollections };

    replicate(replParams);
    verifyDocs(collectionSpecs, docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    for (size_t i = 0; i < collectionCount; ++i) {
        addDocs(collections[i], 5, idPrefix);
        docIDs[i] = getDocIDs(collections[i]);
    }

    replicate(replParams);
    verifyDocs(collectionSpecs, docIDs);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Multiple Collections Incremental Revisions SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Lavenders,
        Tulips
    };
    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, "sguser", "password");
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

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

    ReplParams replParams { replCollections };
    replicate(replParams);
    // total 3 docs, 12 revs, for each collections.
    CHECK(_callbackStatus.progress.documentCount == 36);
    verifyDocs(collectionSpecs, docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull deltas from Collection SG", "[.SyncServerCollection]") {
    constexpr size_t kDocBufSize = 60;
    // CBG-2643 blocking 1000 docs with 1000 props due to replication taking more than ~1sec
    constexpr int kNumDocs = 799, kNumProps = 799;
    const string idPrefix = timePrefix();

    const string docIDPref = idPrefix + "doc";
    vector<string> chIDs {idPrefix+"a"};

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };
    SG::TestUser testUser { _sg, "pdfcsg", chIDs, collectionSpecs };
    _sg.authHeader = testUser.authHeader();
    std::array<C4Collection *, collectionCount> collections
            = collectionPreamble(collectionSpecs, testUser);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i] };
    }

    ReplParams replParams { replCollections };

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        for (size_t i = 0; i < collectionCount; ++i){
            TransactionHelper t(db);
            std::srand(123456); // start random() sequence at a known place
            for (int docNo = 0; docNo < kNumDocs; ++docNo) {
                constexpr size_t kDocBufSize = 60;
                char docID[kDocBufSize];
                snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
                Encoder encPopulate(c4db_createFleeceEncoder(db));
                encPopulate.beginDict();
                encPopulate.writeKey(kC4ReplicatorOptionChannels);
                encPopulate.writeString(chIDs[0]);

                for (int p = 0; p < kNumProps; ++p) {
                    encPopulate.writeKey(format("field%03d", p));
                    encPopulate.writeInt(std::rand());
                }
                encPopulate.endDict();
                alloc_slice body = encPopulate.finish();
                string revID = createNewRev(collections[i], slice(docID), body);
            }
        }
    };

    populateDB();

    C4Log("-------- Pushing to SG --------");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    for (size_t i = 0; i < collectionCount; ++i) {
        JSONEncoder encUpdate;
        encUpdate.beginDict();
        encUpdate.writeKey("docs"_sl);
        encUpdate.beginArray();
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            C4Error error;
            c4::ref<C4Document> doc = c4coll_getDoc(collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            encUpdate.beginDict();
            encUpdate.writeKey("_id"_sl);
            encUpdate.writeString(docID);
            encUpdate.writeKey("_rev"_sl);
            encUpdate.writeString(doc->revID);
            for (Dict::iterator j(props); j; ++j) {
                encUpdate.writeKey(j.keyString());
                if(j.keyString() == kC4ReplicatorOptionChannels){
                    encUpdate.writeString(j.value().asString());
                    continue;
                }
                auto value = j.value().asInt();
                if (RandomNumber() % 8 == 0)
                    value = RandomNumber();
                encUpdate.writeInt(value);
            }
            encUpdate.endDict();
        }
        encUpdate.endArray();
        encUpdate.endDict();

        REQUIRE(_sg.insertBulkDocs(collectionSpecs[i], encUpdate.finish(), 30.0));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(C4STR(kC4ReplicatorOptionDisableDeltas), true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();

        collections = collectionPreamble(collectionSpecs, testUser);
        replParams.setPushPull(kC4Disabled, kC4OneShot);

        populateDB();

        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replicate(replParams);
        double time = st.elapsed();

        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        for (size_t i = 0; i < collectionCount; ++i){
            int n = 0;
            C4Error error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(collections[i], nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while (c4enum_next(e, ERROR_INFO(error))) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                CHECK(slice(info.docID).hasPrefix(slice(docIDPref)));
                CHECK(slice(info.revID).hasPrefix("2-"_sl));
                ++n;
            }
            CHECK(error.code == 0);
            CHECK(n == kNumDocs);
        }
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
        timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push and Pull Attachments SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    // one collection now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Tulips,
        Lavenders
    };
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;

    // Set up replication
    SG::TestUser testUser { _sg, "papasg", { "*" }, collectionSpecs }; // Doesn't use channels
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection*, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};
    std::array<vector<C4BlobKey>, collectionCount> blobKeys; // blobKeys1a, blobKeys1b;

    for(size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i] };
    }
    ReplParams replParams { replCollections };

    vector<string> attachments1 = {
            idPrefix + "Attachment A",
            idPrefix + "Attachment B",
            idPrefix + "Attachment Z"
        };
    {
        string doc1 = idPrefix + "doc1";
        string doc2 = idPrefix + "doc2";
        TransactionHelper t(db);
        for (size_t i = 0; i < collectionCount; ++i) {
            blobKeys[i] = addDocWithAttachments(db, collectionSpecs[i],  slice(doc1), attachments1, "text/plain");
            docIDs[i] = getDocIDs(collections[i]);
        }
    }

    C4Log("-------- Pushing to SG --------");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    C4Log("-------- Checking docs and attachments --------");
    verifyDocs(collectionSpecs, docIDs, true);
    for (size_t i = 0; i < collectionCount; ++i) {
        checkAttachments(verifyDb, blobKeys[i], attachments1);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Push & Pull Deletion SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    const string channelID = idPrefix;

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Tulips,
        Lavenders
    };

    // Set up replication
    SG::TestUser testUser { _sg, "ppdsg", { channelID }, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    const string docID = idPrefix + "ppd-doc1";


    std::array<C4Collection*, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    alloc_slice channelJSON = SG::addChannelToJSON("{}", "channels", { channelID });

    for(size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i] };
        createRev(collections[i], slice(docID), kRevID, json2fleece(channelJSON.asString().c_str()));
        createRev(collections[i], slice(docID), kRev2ID, json2fleece(channelJSON.asString().c_str()), kRevDeleted);
    }

    ReplParams replParams { replCollections };
    replParams.setPushPull(kC4OneShot, kC4Disabled);

    replicate(replParams);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    collections = collectionPreamble(collectionSpecs, testUser);
    replParams.setPushPull(kC4Disabled, kC4OneShot);

    for(size_t i = 0; i < collectionCount; ++i){
        createRev(collections[i], slice(docID), kRevID, kFleeceBody);
    }

    replicate(replParams);

    for(size_t i = 0; i < collectionCount; ++i){
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(collections[i], slice(docID), true, kDocGetAll, nullptr);
        REQUIRE(remoteDoc);
        CHECK(remoteDoc->revID == kRev2ID);
        CHECK((remoteDoc->flags & kDocDeleted) != 0);
        CHECK((remoteDoc->selectedRev.flags & kRevDeleted) != 0);
        REQUIRE(c4doc_selectParentRevision(remoteDoc));
        CHECK(remoteDoc->selectedRev.revID == kRevID);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Resolve Conflict SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();

    // one collection now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Roses,
        Tulips,
        Lavenders
    };
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;

    // Set up replication
    SG::TestUser testUser { _sg, "rcsg", { "*" }, collectionSpecs }; // Doesn't use channels
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection*, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);

    std::vector<C4ReplicationCollection> replCollections {collectionCount};
    std::array<string, collectionCount> collNames;

    for(size_t i = 0; i < collectionCount; ++i) {
        collNames[i] = idPrefix + Options::collectionSpecToPath(collectionSpecs[i]).asString();
    }

    for (size_t i = 0; i < collectionCount; ++i) {
        createFleeceRev(collections[i], slice(collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(collections[i], slice(collNames[i]), revOrVersID("2-12121212", "1@cafe"),
                        "{\"db\":\"remote\"}"_sl);
        docIDs[i] = getDocIDs(collections[i]);
        replCollections[i] = { collectionSpecs[i] };
    }

    // Send the docs to remote
    ReplParams replParams { replCollections };
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);
    verifyDocs(collectionSpecs, docIDs, true);

    deleteAndRecreateDB();
    for (size_t i = 0; i < collectionCount; ++i) {
        collections[i] = c4db_createCollection(db, collectionSpecs[i], ERROR_INFO());
        createFleeceRev(collections[i], slice(collNames[i]), kRev1ID, "{}"_sl);
        createFleeceRev(collections[i], slice(collNames[i]), revOrVersID("2-13131313", "1@babe"),
                        "{\"db\":\"local\"}"_sl);
    }
    collections = collectionPreamble(collectionSpecs, testUser);
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replParams.setDocIDs(docIDs);

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

        C4Document* resolvedDoc = remoteDoc;

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
    replicate(replParams);

    for (size_t i = 0; i < collectionCount; ++i) {
        c4::ref<C4Document> doc = c4coll_getDoc(collections[i], slice(collNames[i]),
                                                        true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK(fleece2json(c4doc_getRevisionBody(doc)) == "{db:\"remote\"}"); // Remote Wins
        REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Update Once-Conflicted Doc - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "uocd-doc";

    // one collection now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };

    // Set up replication
    vector<string> chIDs { idPrefix + "uocd" };
    SG::TestUser testUser { _sg, "uocd", chIDs, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    // Create a conflicted doc on SG, and resolve the conflict
    std::array<std::string, 4> bodies {
            R"({"_rev":"1-aaaa","foo":1})",
            R"({"_revisions":{"start":2,"ids":["bbbb","aaaa"]},"foo":2.1})",
            R"({"_revisions":{"start":2,"ids":["cccc","aaaa"]},"foo":2.2})",
            R"({"_revisions":{"start":3,"ids":["dddd","cccc"]},"_deleted":true})"
    };

    for(const auto& b : bodies) {
        _sg.upsertDoc(collectionSpecs[0], docID + "?new_edits=false", b, chIDs);
    }

    // Set up pull replication
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(size_t i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i] };
    }

    ReplParams replParams { replCollections };
    replParams.setPushPull(kC4Disabled, kC4OneShot);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    // Verify doc:
    c4::ref<C4Document> doc = c4coll_getDoc(collections[0], slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    auto body = SG::addChannelToJSON(R"({"ans*wer":42})"_sl, "channels"_sl, chIDs);
    {
        TransactionHelper t { db };
        body = c4db_encodeJSON(db, body, ERROR_INFO());
    }

    createRev(collections[0], slice(docID), "3-ffff"_sl, body);

    // Push replication set-up
    replParams.setPushPull(kC4OneShot, kC4Disabled);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replicate(replParams);

    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs {
            { getDocIDs(collections[0]) }
    };

    verifyDocs(collectionSpecs, docIDs, true);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Revoked Revision - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apefrr-doc1";
    const string channelID = idPrefix + "a";

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };

    SG::TestUser testUser { _sg, "apefrrsg", { channelID }, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    // Setup pull filter to filter the removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };

    std::array<C4Collection*, collectionCount> collections =
            collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections { collectionCount };

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = {
                collectionSpecs[i], kC4Disabled, kC4OneShot,
                nullslice, nullptr, _pullFilter, this
        };
    }

    ReplParams replParams { replCollections };

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

    for(auto& spec : collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, docIDstr, "{}", { channelID }));
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    // Verify:
    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
    }
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify if doc1 is not purged as the revoked rev is filtered:
    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
    }
    // The two below checks are failing, because of CBG-2487
    CHECK(_docsEnded == collectionCount);
    CHECK(_counter == collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Revoke Access - SGColl", "[.SyncServerCollection]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "a";
    const string channelIDb = idPrefix + "b";

    constexpr size_t collectionCount = 3;
    const std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };

    // Create a temporary user for this test
    SG::TestUser testUser { _sg, "aperasg", { channelIDa, channelIDb }, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

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
    std::array<C4Collection*, collectionCount> collections =
            collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = {
                collectionSpecs[i], kC4Disabled, kC4OneShot,
                nullslice, nullptr, _pullFilter, this
        };
    }

    ReplParams replParams { replCollections };

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

    // Put doc in remote DB, in channels a and b
    for(auto& spec : collectionSpecs) {
        REQUIRE(_sg.upsertDoc( spec, docIDstr, "{}", { channelIDa, channelIDb } ));
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to channel 'a':
    REQUIRE(testUser.setChannels({ channelIDb }));

    for(int i = 0; i < collectionCount; ++i) {
        // Verify
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
        CHECK(slice(doc1->revID).hasPrefix("1-"_sl));

        // Update doc to only channel 'b'
        auto oRevID = slice(doc1->revID).asString();
        REQUIRE(_sg.upsertDoc(collectionSpecs[i], docIDstr, oRevID, "{}", { channelIDb }));
    }

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        REQUIRE(doc1);
        CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    }
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify that doc1 is purged:
    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(docIDstr), true, kDocGetAll, nullptr);
        // This check is currently failing because of CBG-2487
        REQUIRE(!doc1);
    }

    CHECK(_docsEnded == collectionCount);
    CHECK(_counter == collectionCount);
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
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;

    // Set up replication
    SG::TestUser testUser { _sg, "pdfcsg",  { "*" }, collectionSpecs }; // Doesn't use channels
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection*, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    encContextMap.reset(new CipherContextMap);
    decContextMap.reset(new CipherContextMap);

    std::array<string, collectionCount> docs;
    for(size_t i = 0; i < collectionCount; ++i) {
        docs[i] = idPrefix + Options::collectionSpecToPath(collectionSpecs[i]).asString();
    }
    slice originalJSON = R"({"xNum":{"@type":"encryptable","value":"123-45-6789"}})"_sl;

    {
        TransactionHelper t(db);
        for (size_t i = 0; i < collectionCount; ++i) {
            createFleeceRev(collections[i], slice(docs[i]), kRevID, originalJSON);
            docIDs[i] = getDocIDs(collections[i]);
            replCollections[i] = { collectionSpecs[i] };
            encContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(collectionSpecs[i]),
                                   std::forward_as_tuple(collections[i], docs[i].c_str(), "xNum", false));
            decContextMap->emplace(std::piecewise_construct,
                                   std::forward_as_tuple(collectionSpecs[i]),
                                   std::forward_as_tuple(collections[i], docs[i].c_str(), "xNum", false));
        }
    }

    ReplParams replParams { replCollections };
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);

    replicate(replParams);
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
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
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
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984};
    }
    REQUIRE(Address::isSecure(_sg.address));

    // One-shot push setup
    constexpr size_t collectionCount = 1;
    const std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses
    };
    collectionPreamble(collectionSpecs, "sguser", "password");
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i], kC4OneShot, kC4Disabled };
    }

    ReplParams replParams { replCollections };
    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    replicate(replParams);

    // Intermediate cert (Replicator/tests/data/cert/sg_cert.pem (2nd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
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

    replicate(replParams);

    // Root cert (Replicator/tests/data/cert/sg_cert.pem (3rd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
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

    replicate(replParams);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pinned Certificate Failure - SGColl", "[.SyncServerCollection]") {
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984 };
    }
    REQUIRE(Address::isSecure(_sg.address));

    // Using an unmatched pinned cert:
    _sg.pinnedCert =                                                               \
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
    collectionPreamble(collectionSpecs, "sguser", "password");
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i], kC4OneShot, kC4Disabled };
    }

    ReplParams replParams { replCollections };

    // expectSuccess = false so we can check the error code
    replicate(replParams, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif //#ifdef COUCHBASE_ENTERPRISE

// !Note! Not passing, pending CBG-2487
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Disabled - Revoke Access SG",
                 "[.SyncServerCollection]") {
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
        Tulips,
        Roses,
        Lavenders
    };

    string idPrefix = timePrefix();
    string doc1ID = idPrefix + "doc1";
    vector<string> chIDs {idPrefix};
    constexpr const char* uname = "apdra";
    SG::TestUser user {_sg, uname, chIDs, collectionSpecs};
    _sg.authHeader = user.authHeader();
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, user);
    std::array<C4ReplicationCollection, collectionCount> replCollections;

    for (auto collSpec : collectionSpecs) {
        REQUIRE(_sg.upsertDoc(collSpec, doc1ID, "{}"_sl, chIDs));
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
    } cbContext[collectionCount];

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](
        C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict, void *context)
    {
        CBContext* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ((flags & kRevPurged) == kRevPurged) {
            ctx->pullFilterPurge++;
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
            &cbContext[i]     // callbackContext
        };
    }

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
    std::vector<AllocedDict> allocedDicts;
    C4ParamsSetter paramsSetter
        = [&replCollections, &allocedDicts](C4ReplicatorParameters& c4Params)
    {
        c4Params.collectionCount = replCollections.size();
        c4Params.collections     = replCollections.data();
        fleece::Encoder enc;
        enc.writeBool(false);
        Doc doc {enc.finish()};
        allocedDicts.emplace_back(
            repl::Options::updateProperties(
                AllocedDict(c4Params.optionsDictFleece),
                C4STR(kC4ReplicatorOptionAutoPurge),
                doc.root())
            );
        c4Params.optionsDictFleece = allocedDicts.back().data();
    };
    replicate(paramsSetter);

    for (size_t i = 0; i < collectionCount; ++i) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(cbContext[i].docsEndedTotal == 1);
        CHECK(cbContext[i].docsEndedPurge == 0);
        CHECK(cbContext[i].pullFilterTotal == 1);
        CHECK(cbContext[i].pullFilterPurge == 0);
    }

    // Revoke access to all channels:
    REQUIRE(user.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    for (auto& c: cbContext) {
        c.reset();
    }

    replicate(paramsSetter);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    for (size_t i = 0; i < collectionCount; ++i) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        // This check pending CBG-2487
        CHECK(cbContext[i].docsEndedPurge == 1);
        // No pull filter called
        CHECK(cbContext[i].pullFilterTotal == 0);
    }
}


TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Remove Doc From Channel SG", "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    string        doc1ID {idPrefix + "doc1"};
    vector<string> chIDs {idPrefix+"a", idPrefix+"b"};

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
            Roses,
            Tulips,
            Lavenders
    };

    SG::TestUser testUser { _sg, "rdfcsg", chIDs, collectionSpecs };

    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    // Create docs on SG:
    _sg.authHeader = testUser.authHeader();
    for (size_t i = 0; i < collectionCount; ++i) {
        _sg.upsertDoc(collectionSpecs[i], doc1ID, "{}"_sl, chIDs);
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
    ReplParams replParams { replCollections };
    SECTION("Auto Purge Enabled") {
        autoPurgeEnabled = true;
    }

    SECTION("Auto Purge Disabled") {
        replParams.setOption(C4STR(kC4ReplicatorOptionAutoPurge), false);
        autoPurgeEnabled = false;
    }

    replicate(replParams);

    CHECK(context.docsEndedTotal == collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for(int i = 0; i < collectionCount; ++i) {
        // Verify doc
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(c4rev_getGeneration(doc1->revID) == 1);

        // Once verified, remove it from channel 'a' in that collection
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", { chIDs[1] });
    }

    C4Log("-------- Pull update");
    context.reset();
    replicate(replParams);

    CHECK(context.docsEndedTotal == collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == collectionCount);
    CHECK(context.pullFilterPurge == 0);

    for(int i = 0; i < collectionCount; ++i) {
        // Verify the update:
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
        CHECK(c4rev_getGeneration(doc1->revID) == 2);

        // Remove doc from all channels:
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", {});
    }

    C4Log("-------- Pull the removed");
    context.reset();
    replicate(replParams);

    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);

        if (autoPurgeEnabled) {
            // Verify if doc1 is purged:
            REQUIRE(!doc1);
        } else {
            REQUIRE(doc1);
        }
    }

    CHECK(context.docsEndedPurge == collectionCount);
    if(autoPurgeEnabled) {
        CHECK(context.pullFilterPurge == collectionCount);
    } else {
        // No pull filter called
        CHECK(context.pullFilterTotal == 0);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled - Filter Removed Revision SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    // one collection for now. Will use multiple collection when SG is ready.
    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs = {
        Roses,
        Tulips,
        Lavenders
    };
    string doc1ID = idPrefix + "doc1";
    vector<string> chIDs {idPrefix+"a"};

    SG::TestUser testUser {_sg, kTestUserName, chIDs, collectionSpecs };

    _sg.authHeader = testUser.authHeader();
    std::array<C4Collection*, collectionCount> collections =
        collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections{collectionCount};

    // Create docs on SG:
    for (size_t i = 0; i < collectionCount; ++i) {
        REQUIRE(_sg.upsertDoc(collectionSpecs[i], doc1ID, "{}"_sl, chIDs));
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
    ReplParams replParams { replCollections };
    replicate(replParams);

    CHECK(cbContext.docsEndedTotal == collectionCount);
    CHECK(cbContext.docsEndedPurge == 0);
    CHECK(cbContext.pullFilterTotal == collectionCount);
    CHECK(cbContext.pullFilterPurge == 0);

    for(int i = 0; i < collectionCount; ++i) {
        // Verify
        c4::ref<C4Document> doc1 = c4coll_getDoc(collections[i], slice(doc1ID),
                                                 true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);

        // Remove doc from all channels
        auto oRevID = slice(doc1->revID).asString();
        _sg.upsertDoc(collectionSpecs[i], doc1ID, R"({"_rev":")" + oRevID + "\"}", {});
    }

    C4Log("-------- Pull the removed");
    cbContext.reset();
    replicate(replParams);

    // Verify if doc1 is not purged as the removed rev is filtered:
    for(auto& coll : collections) {
        c4::ref<C4Document> doc1 = c4coll_getDoc(coll, slice(doc1ID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc1);
    }
    CHECK(cbContext.docsEndedPurge == collectionCount);
    CHECK(cbContext.pullFilterPurge == collectionCount);
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Auto Purge Enabled(default) - Delete Doc or Delete then Create Doc SG",
                 "[.SyncServerCollection]") {
    string idPrefix = timePrefix();
    constexpr size_t collectionCount = 3;
    string docID = idPrefix + "doc";
    vector<string> chIDs {idPrefix+"a"};

    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
            Roses,
            Tulips,
            Lavenders
    };
    SG::TestUser testUser {_sg, kTestUserName, chIDs, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i], kC4OneShot, kC4Disabled };
    }

    alloc_slice bodyJSON = SG::addChannelToJSON("{}"_sl, "channels"_sl, chIDs);

    // Create a doc in each collection
    std::array<c4::ref<C4Document>, collectionCount> docs;
    {
        TransactionHelper t(db);
        C4Error error;
        for (size_t i = 0; i < collectionCount; ++i) {
            docs[i] = c4coll_createDoc(collections[i], slice(docID),
                                       json2fleece(bodyJSON.asString().c_str()),
                                       0, ERROR_INFO(error));
            REQUIRE(error.code == 0);
            REQUIRE(docs[i]);
        }
    }
    for (auto& coll : collections) {
        REQUIRE(c4coll_getDocumentCount(coll) == 1);
    }

    // Push parameter
    ReplParams replParams { replCollections };
    // Push to the remote
    replicate(replParams);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error error;
        for (auto& doc : docs) {
            doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
            REQUIRE(error.code == 0);
        }
    }
    // Verify docs are deleted
    for (size_t i = 0; i < collectionCount; ++i) {
        REQUIRE(docs[i]);
        REQUIRE(docs[i]->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
        REQUIRE(c4coll_getDocumentCount(collections[i]) == 0);
    }
    // Push the deleted docs
    replicate(replParams);

    bool deleteThenCreate = true;
    SECTION("Delete then Create Doc") {
        // Create a new doc with the same id that was deleted:
        {
            TransactionHelper t(db);
            for (size_t i = 0; i < collectionCount; ++i) {
                C4Error error;
                docs[i] = c4coll_createDoc(collections[i], slice(docID),
                                           json2fleece(bodyJSON.asString().c_str()),
                                           0, ERROR_INFO(error));
                REQUIRE(error.code == 0);
                REQUIRE(docs[i] != nullptr);
            }
        }
        for (auto coll : collections) {
            REQUIRE(c4coll_getDocumentCount(coll) == 1);
        }
    }

    SECTION("Delete Doc") {
        deleteThenCreate = false;
    }

    // Perform Pull
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    if (deleteThenCreate) {
        for (size_t i = 0; i < collectionCount; ++i) {
            C4Error error;
            c4::ref<C4Document> doc2 = c4coll_getDoc(collections[i], slice(docID), true, kDocGetAll, ERROR_INFO(error));
            CHECK(error.code == 0);
            CHECK(doc2 != nullptr);
            CHECK(doc2->revID == docs[i]->revID);
            CHECK(c4coll_getDocumentCount(collections[i]) == 1);
        }
    } else {
        for (size_t i = 0; i < collectionCount; ++i) {
            C4Error error;
            c4::ref<C4Document> doc2 = c4coll_getDoc(collections[i], slice(docID), true,
                                                     kDocGetAll, ERROR_INFO(error));
            CHECK(error.code == 0);
            CHECK(doc2 != nullptr);
            CHECK(doc2->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
            CHECK(c4coll_getDocumentCount(collections[i]) == 0);
        }
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "API Push Conflict SG", "[.SyncServerCollection]") {
    const string originalRevID = "1-3cb9cfb09f3f0b5142e618553966ab73539b8888";
    const string idPrefix = timePrefix();
    const string doc13ID = idPrefix + "0000013";

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
            Roses,
            Tulips,
            Lavenders
    };

    SG::TestUser testUser { _sg, "apipcsg", { "*" }, collectionSpecs };

    std::array<C4Collection *, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs;
    auto docIDIter = docIDs.begin();
    for (auto coll : collections) {
        importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 0, idPrefix);
        *docIDIter++ = getDocIDs(coll);
    }

    // Set up replCollections
    std::vector<C4ReplicationCollection> replCollections {collectionCount};
    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i], kC4OneShot, kC4Disabled };
    }
    // Push to remote
    ReplParams replParams { replCollections };
    replicate(replParams);

    // Update doc 13 on the remote
    string body = "{\"_rev\":\"" + originalRevID + "\",\"serverSideUpdate\":true}";
    _sg.authHeader = testUser.authHeader();
    for(auto& spec : collectionSpecs) {
        REQUIRE(_sg.upsertDoc(spec, doc13ID, slice(body), {}));
    }


    for(auto& coll : collections) {
        // Create a conflict doc13 at local
        createRev(coll, slice(doc13ID), "2-f000"_sl, kFleeceBody);
        // Verify doc
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(doc13ID), true,
                                                kDocGetAll, nullptr);
        REQUIRE(doc);
        C4Slice revID = C4STR("2-f000");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
    }


    C4Log("-------- Pushing Again (conflict) --------");
    _expectedDocPushErrors = {doc13ID};
    replicate(replParams);

    C4Log("-------- Pulling --------");
    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replParams.setDocIDs(docIDs);

    _expectedDocPushErrors = { };
    _expectedDocPullErrors = {doc13ID};
    replicate(replParams);

    C4Log("-------- Checking Conflict --------");
    for(auto& coll : collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(doc13ID), true, kDocGetAll, nullptr);
        REQUIRE(doc);
        CHECK((doc->flags & kDocConflicted) != 0);
        C4Slice revID = C4STR("2-f000");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
        CHECK(c4doc_getProperties(doc) != nullptr);
        CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
        REQUIRE(c4doc_selectCurrentRevision(doc));
        REQUIRE(c4doc_selectNextRevision(doc));
        revID = C4STR("2-883a2dacc15171a466f76b9d2c39669b");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
        CHECK(c4doc_getProperties(doc) != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        revID = slice(originalRevID);
        CHECK(doc->selectedRev.revID == revID);
    }
}

TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull multiply-updated SG",
                 "[.SyncServerCollection]") {
    // From <https://github.com/couchbase/couchbase-lite-core/issues/652>:
    // 1. Setup CB cluster & Configure SG
    // 2. Create a document using POST API via SG
    // 3. Create a cblite db on local server using cblite serve
    //      ./cblite/build/cblite serve  --create db.cblite2
    // 4. Replicate between SG -> db.cblite2
    //      ./cblite/build/cblite pull  ws://172.23.100.204:4985/db db.cblite2
    // 5. Validate number of records on db.cblite2 ->Should be  equal to number of documents created in Step2
    // 6. Update existing document using update API via SG (more than twice)
    //      PUT sghost:4985/bd/doc_id?=rev_id
    // 7. run replication between SG -> db.cblite2 again
    // This test must use docID filter instead of channel filter, because channel affects digest-based revID

    const string idPrefix = timePrefix();
    const string docID = idPrefix + "doc";

    constexpr size_t collectionCount = 3;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
            Roses,
            Tulips,
            Lavenders
    };

    SG::TestUser testUser { _sg, "pmusg", { "*" }, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection*, collectionCount> collections
        = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};
    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = {collectionSpecs[i], kC4Disabled, kC4OneShot};
        _sg.upsertDoc(collectionSpecs[i], docID + "?new_edits=false",
                      R"({"count":1, "_rev":"1-1111"})");
    }

    std::array<unordered_map<alloc_slice, unsigned>, collectionCount> docIDs {
        unordered_map<alloc_slice, unsigned> {{ alloc_slice(docID), 0 }},
        unordered_map<alloc_slice, unsigned> {{ alloc_slice(docID), 0 }},
        unordered_map<alloc_slice, unsigned> {{ alloc_slice(docID), 0 }}
    };

    ReplParams replParams { replCollections };
    replParams.setDocIDs(docIDs);
    replicate(replParams);
    // This seems to return an increasing number on each run? Commenting out for now
//    CHECK(_callbackStatus.progress.documentCount == 3);
    for(auto& coll : collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID),
                                                true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc);
        CHECK(doc->revID == "1-1111"_sl);
    }

    const std::array<std::string, 3> bodies {
            R"({"count":2, "_rev":"1-1111"})",
            R"({"count":3, "_rev":"2-c5557c751fcbfe4cd1f7221085d9ff70"})",
            R"({"count":4, "_rev":"3-2284e35327a3628df1ca8161edc78999"})"
    };

    for(auto& spec : collectionSpecs) {
        for(const auto& body : bodies) {
            _sg.upsertDoc(spec, docID, body);
        }
    }

    replicate(replParams);
    for(auto& coll : collections) {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, slice(docID), true, kDocGetCurrentRev, nullptr);
        REQUIRE(doc);
        CHECK(doc->revID == "4-ffa3011c5ade4ec3a3ec5fe2296605ce"_sl);
    }
}
// This test takes > 1 minute per collection, so I have given it "SyncCollSlow" tag
TEST_CASE_METHOD(ReplicatorCollectionSGTest, "Pull iTunes deltas from Collection SG", "[.SyncCollSlow]") {
    string idPrefix = timePrefix() + "pidfsg";

    constexpr size_t collectionCount = 2;
    std::array<C4CollectionSpec, collectionCount> collectionSpecs {
            Roses,
            Tulips
    };

    // Set up replication
    SG::TestUser testUser { _sg, "pidfsgc", { "*" }, collectionSpecs };
    _sg.authHeader = testUser.authHeader();

    std::array<C4Collection*, collectionCount> collections
            = collectionPreamble(collectionSpecs, testUser);
    std::vector<C4ReplicationCollection> replCollections {collectionCount};

    for(int i = 0; i < collectionCount; ++i) {
        replCollections[i] = { collectionSpecs[i] };
    }

    ReplParams replParams { replCollections };

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        for(auto& coll : collections) { // Import 5000 docs per collection
            importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", coll, 0, false, 900, idPrefix);
        }
    };
    populateDB();

    // Filter replication by docID
    std::array<std::unordered_map<alloc_slice, unsigned>, collectionCount> docIDs { };
    for(int i = 0; i < collectionCount; ++i) {
        docIDs[i] = getDocIDs(collections[i]);
    }
    replParams.setDocIDs( docIDs );

    C4Log("-------- Pushing to SG --------");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    for(int i = 0; i < collectionCount; ++i) {
        auto numDocs = c4coll_getDocumentCount(collections[i]);
        constexpr size_t docBufSize = 50;
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < numDocs; ++docNo) {
            char docID[docBufSize];
            snprintf(docID, docBufSize, "%s%07u", idPrefix.c_str(), docNo+1);
            C4Error error;
            c4::ref<C4Document> doc = c4coll_getDoc(collections[i], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator it(props); it; ++it) {
                enc.writeKey(it.keyString());
                auto value = it.value();
                if (it.keyString() == "Play Count"_sl)
                    enc.writeInt(value.asInt() + 1);
                else
                    enc.writeValue(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        _sg.insertBulkDocs(collectionSpecs[i], enc.finish(), 300);
    }

    uint64_t numDocs = 0;
    for(auto& coll : collections) {
        numDocs += c4coll_getDocumentCount(coll);
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(kC4ReplicatorOptionDisableDeltas, true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        collections = collectionPreamble(collectionSpecs, testUser);
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        Stopwatch st;
        replicate(replParams);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;
        // Verify docs
        int n = 0;
        for(auto& coll : collections) {
            C4Error error;
            c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(coll, nullptr, ERROR_INFO(error));
            REQUIRE(e);
            while (c4enum_next(e, ERROR_INFO(error))) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                auto revID = slice(info.revID);
                CHECK(revID.hasPrefix("2-"_sl));
                ++n;
            }
            CHECK(error.code == 0);
        }
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}
