//
// Created by Callum Birks on 05/01/2023.
//

#ifndef LITECORE_REPLICATORCOLLECTIONSGTEST_HH
#define LITECORE_REPLICATORCOLLECTIONSGTEST_HH

#include "c4Base.h"
#include "ReplicatorAPITest.hh"
#include "Defer.hh"
#include "c4Collection.h"
#include "c4DocEnumerator.h"
#include "c4Document.h"
#include "c4Database.h"
#include "SGTestUser.hh"
#include "ReplParams.hh"
#include <array>
#include <c4Collection.hh>
#include <cstdint>
#include <iostream>
#include <typeinfo>

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

static constexpr const char* kTestUserName = "sguser";

class ReplicatorCollectionSGTest : public ReplicatorAPITest {
  public:
    explicit ReplicatorCollectionSGTest(const std::string& minSGVer = "3.1", const std::string& maxSGVer = "",
                                        uint16_t port = 4984, const slice remoteDBName = kScratchDBName)
        : ReplicatorAPITest(VersionVectorOption), _expectedSGVersion{minSGVer, maxSGVer} {
        _sg.pinnedCert = C4Test::readFile("Replicator/tests/data/cert/cert.pem");
        if ( getenv("NOTLS") ) {
            _sg.address      = {kC4Replicator2Scheme, C4STR("localhost"), port};
            _sg.remoteDBName = remoteDBName;
        } else {
            _sg.address      = {kC4Replicator2TLSScheme, C4STR("localhost"), port};
            _sg.remoteDBName = remoteDBName;
        }
        _flushedScratch = true;
    }

    ~ReplicatorCollectionSGTest() {
        if ( verifyDb != nullptr ) {
            bool deletedDb = c4db_delete(verifyDb, ERROR_INFO());
            REQUIRE(deletedDb);
            c4db_release(verifyDb);
            verifyDb = nullptr;
        }
    }

    // Database verifyDb:
    C4Database* verifyDb{nullptr};

    void resetVerifyDb() {
        if ( verifyDb == nullptr ) {
            verifyDb = createDatabase("verifyDb");
        } else {
            deleteAndRecreateDB(verifyDb);
        }
    }

#ifdef COUCHBASE_ENTERPRISE
    static C4SliceResult propEncryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                       C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                       C4StringResult* outKeyID, C4Error* outError);

    static C4SliceResult propDecryptor(void* ctx, C4CollectionSpec spec, C4String docID, FLDict properties,
                                       C4String keyPath, C4Slice input, C4String algorithm, C4String keyID,
                                       C4Error* outError);
#endif

    static AllocedDict createOptionsAuth(const std::string& username, const std::string& password) {
        Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorAuthType));
        enc.writeString("Basic"_sl);
        enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
        enc.writeString(username);
        enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
        enc.writeString(password);
        enc.endDict();
        enc.endDict();
        return AllocedDict(enc.finish());
    }

    // This function should be called before replicating against the Couchbase server.
    // It does the following:
    //  - sets up _options for authenticaton
    //  - assigns _collection with input "collection"
    //  - creates the collection if it is not the default collection
    //  - sets up the log level with input "logLevel"
    //  - returns the C4Collection object.
    std::vector<C4Collection*> collectionPreamble(const std::vector<C4CollectionSpec>& collectionSpecs) {
        std::vector<C4Collection*> ret{collectionSpecs.size()};
        for ( size_t i = 0; i < collectionSpecs.size(); ++i ) {
            if ( kC4DefaultCollectionSpec != collectionSpecs[i] ) { db->createCollection(collectionSpecs[i]); }
            ret[i] = db->getCollection(collectionSpecs[i]);
        }
        return ret;
    }

    // propertyEncryption: 0, no encryption; 1, encryption only; 2, encryption and decryption
    void verifyDocs(const std::vector<std::unordered_map<alloc_slice, uint64_t>>& docIDs, bool checkRev = false,
                    int propertyEncryption = 0) {
        REQUIRE((!_collectionSpecs.empty() && !_collections.empty()));
        resetVerifyDb();
        std::vector<C4Collection*> collections{_collectionCount};
        for ( size_t i = 0; i < _collectionCount; ++i ) {
            if ( _collectionSpecs[i] != Default ) { verifyDb->createCollection(_collectionSpecs[i]); }
            collections[i] = verifyDb->getCollection(_collectionSpecs[i]);
            REQUIRE(collections[i]);
            CHECK(0 == c4coll_getDocumentCount(collections[i]));
        }

        // Pull to verify that Push successfully pushed all documents in docIDs

        ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
        replParams.setDocIDs(docIDs);
#ifdef COUCHBASE_ENTERPRISE
        if ( propertyEncryption > 0 ) {
            replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);
        }
        if ( propertyEncryption == 1 ) {
            replParams.setOption(kC4ReplicatorOptionDisablePropertyDecryption, true);
            std::for_each(decContextMap->begin(), decContextMap->end(), [this](auto& p) {
                p.second.collection = c4db_getCollection(verifyDb, p.first, ERROR_INFO());
            });
        }
#else
        (void)propertyEncryption;
#endif
        {
            C4Database* savedb = db;
            DEFER { db = savedb; };
            db = verifyDb;
            replicate(replParams);
        }

        for ( size_t i = 0; i < _collectionCount; ++i ) {
            if ( checkRev ) {
                c4::ref<C4DocEnumerator> e     = c4coll_enumerateAllDocs(collections[i], nullptr, ERROR_INFO());
                unsigned                 count = 0;
                while ( c4enum_next(e, ERROR_INFO()) ) {
                    ++count;
                    C4DocumentInfo info;
                    c4enum_getDocumentInfo(e, &info);
                    auto it = docIDs[i].find(info.docID);
                    CHECK(it != docIDs[i].end());
                    CHECK(it->second == c4rev_getTimestamp(info.revID));
                }
                CHECK(count == docIDs[i].size());
            } else {
                auto count = c4coll_getDocumentCount(collections[i]);
                REQUIRE(count == docIDs[i].size());
            }
        }
    }

    // map: docID -> rev generation
    static std::unordered_map<alloc_slice, uint64_t> getDocIDs(C4Collection* collection) {
        std::unordered_map<alloc_slice, uint64_t> ret;
        c4::ref<C4DocEnumerator>                  e = c4coll_enumerateAllDocs(collection, nullptr, ERROR_INFO());
        {
            while ( c4enum_next(e, ERROR_INFO()) ) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                ret.emplace(info.docID, c4rev_getTimestamp(info.revID));
            }
        }
        return ret;
    }

    // Delete and recreate DB, also recreate the collections in the clean DB
    void deleteAndRecreateDBAndCollections() {
        REQUIRE((!_collectionSpecs.empty() && !_collections.empty()));
        deleteAndRecreateDB();
        _collections = collectionPreamble(_collectionSpecs);
    }

    bool sgVersionCheck() {
        alloc_slice       serverName = _sg.getServerName();
        const std::string minSGVer   = std::string("Couchbase Sync Gateway/") + _expectedSGVersion.first;
        const std::string maxSGVer   = std::string("Couchbase Sync Gateway/") + _expectedSGVersion.second;
        // Compares the ASCII bytes of the version strings
        bool isCorrectVersion = serverName >= minSGVer && (_expectedSGVersion.second.empty() || serverName < maxSGVer);
        if ( !isCorrectVersion ) {
            C4Log("[ReplicatorTest] Sync Gateway version differs from expected."
                  "\n\tExpected version: \n\t\tMinimum (inclusive): %s"
                  "\n\t\tMaximum (exclusive): %s\n\tActual version: %.*s",
                  minSGVer.c_str(), maxSGVer.c_str(), SPLAT(serverName));
        } else {
            C4Log("[ReplicatorTest] Sync Gateway version is: %.*s", SPLAT(serverName));
        }
        return isCorrectVersion;
    }

    /*
     * Set up test parameters (collectionSpecs, collections, testUser, user auth).
     * Creates all the collections in the db, specified by collSpecs. Creates a user on SG with access to the
     * specified channels in the specified collections. Sets up the _sg.authHeader for user authorisation for
     * `SG` class REST requests.
     * Easiest usage of the function is i.e. `initTest({ Roses, Tulips, Lavenders })` which will create those 3
     * collections in the db and set up the SG user with access to all channels in those collections.
     * Once called, test parameters can be access by; _collectionSpecs, _collections, _collectionCount,
     * _testUser and _docIDs.
     * If you're using a channel filter, you can do i.e. `initTest({ Roses, Tulips, Lavenders }, { channel1, channel2 })`
     */
    void initTest(const std::vector<C4CollectionSpec>& collSpecs, const std::vector<std::string>& channelIDs = {"*"},
                  const std::string& username = kTestUserName) {
        _collectionSpecs = collSpecs;
        _collections     = collectionPreamble(collSpecs);
        _collectionCount = _collectionSpecs.size();
        // Avoid copy constructor
        new (&_testUser) SG::TestUser{_sg, username, channelIDs, collSpecs};
        _options       = createOptionsAuth(_testUser._username, _testUser._password);
        _sg.authHeader = _testUser.authHeader();
        _sg.useRevTrees = isRevTrees();
        _docIDs.resize(_collectionCount);
        REQUIRE(sgVersionCheck());
    }

    /*
     * Fetch the docIDs from all collections and update _docIDs with the result.
     * If you're using a docID filter, you can call:
     *   updateDocIDs();
     *   replParams.setDocIDs(_docIDs);
     * to fetch the latest docIDs and update the docID filter on your ReplParams
     */
    void updateDocIDs() {
        // Check the initTest
        REQUIRE((!_collectionSpecs.empty() && !_collections.empty()));
        for ( int i = 0; i < _collectionCount; ++i ) { _docIDs[i] = getDocIDs(_collections[i]); }
    }

    alloc_slice getLegacyRevID(C4Collection* coll, slice docID) {
        c4::ref doc = c4coll_getDoc(coll, docID, true, kDocGetCurrentRev, ERROR_INFO());
        return getLegacyRevID(coll->getSpec(), doc);
    }

    alloc_slice getLegacyRevID(C4CollectionSpec spec, C4Document* doc) {
        alloc_slice revID;
        if (isRevTrees()) {
            revID = doc->revID;
        } else {
            alloc_slice docID = doc->docID;
            revID = _sg.getRevID(docID.asString(), spec);
        }
        return revID;
    }

    alloc_slice getLegacyRevID(C4CollectionSpec spec, const C4DocumentInfo& info) {
        alloc_slice revID;
        if (isRevTrees()) {
            revID = info.revID;
        } else {
            alloc_slice docID = info.docID;
            revID = _sg.getRevID(docID.asString(), spec);
        }
        return revID;
    }

    struct CipherContext {
        C4Collection*          collection;
        slice                  docID;
        slice                  keyPath;
        int                    called{0};
        std::optional<C4Error> simulateError;

        CipherContext(C4Collection* c, const char* id, const char* path) : collection(c), docID(id), keyPath(path) {}
    };

    using CipherContextMap = std::unordered_map<C4CollectionSpec, CipherContext>;
    std::unique_ptr<CipherContextMap> encContextMap;
    std::unique_ptr<CipherContextMap> decContextMap;

    std::vector<C4CollectionSpec>                          _collectionSpecs{};
    std::vector<C4Collection*>                             _collections{};
    SG::TestUser                                           _testUser{};
    size_t                                                 _collectionCount = 0;
    std::vector<std::unordered_map<alloc_slice, uint64_t>> _docIDs{};
    // Pair of strings representing min and max SG version
    // min is inclusive, max is exclusive. Empty string for max will ignore it
    // i.e.: {"3.0", "3.1"} represents anything 3.0 and above, and excludes 3.1 and above
    std::pair<std::string, std::string> _expectedSGVersion;
};

#endif  //LITECORE_REPLICATORCOLLECTIONSGTEST_HH
