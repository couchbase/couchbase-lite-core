//
// Created by Callum Birks on 24/01/2023.
//

#pragma once

#include "ReplicatorAPITest.hh"
#include "ReplParams.hh"
#include "SGTestUser.hh"
#include "Defer.hh"
#include "c4DocEnumerator.h"
#include <unordered_map>

#ifdef COUCHBASE_ENTERPRISE
static C4SliceResult propEncryptor(void* ctx, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError);

static C4SliceResult propDecryptor(void* ctx, C4String docID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm,
                                   C4String keyID, C4Error* outError);
#endif

static constexpr const char *kTestUserName = "sguser";

class ReplicatorSGTest : public ReplicatorAPITest {
public:
    ReplicatorSGTest()
    : ReplicatorAPITest() {
        _sg.pinnedCert = C4Test::readFile(sReplicatorFixturesDir + "cert/cert.pem");
        if(getenv("NOTLS")) {
            _sg.address = {kC4Replicator2Scheme,
                           C4STR("localhost"),
                           4984};
        } else {
            _sg.address = {kC4Replicator2TLSScheme,
                           C4STR("localhost"),
                           4984};
        }

        _flushedScratch = true;
    }
    ~ReplicatorSGTest() {
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

    // propertyEncryption: 0, no encryption; 1, encryption only; 2, encryption and decryption
    void verifyDocs(const std::unordered_map<alloc_slice, unsigned>& docIDs,
                    bool checkRev =false, int propertyEncryption =0) {
        resetVerifyDb();

        // Pull to verify that Push successfully pushed all documents in docIDs
        ReplParams replParams { kC4Disabled, kC4OneShot };
        replParams.setDocIDs(docIDs);
#ifdef COUCHBASE_ENTERPRISE
        if(propertyEncryption > 0) {
            replParams.setPropertyEncryptor(propEncryptor).setPropertyDecryptor(propDecryptor);
        }
        if (propertyEncryption == 1) {
            replParams.setOption(kC4ReplicatorOptionDisablePropertyDecryption, true);
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

        if (checkRev) {
            c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(verifyDb, nullptr, ERROR_INFO());
            unsigned count = 0;
            while (c4enum_next(e, ERROR_INFO())) {
                ++count;
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                auto it = docIDs.find(info.docID);
                CHECK(it != docIDs.end());
                CHECK(it->second == c4rev_getGeneration(info.revID));
            }
            CHECK(count == docIDs.size());
        } else {
            auto count = c4db_getDocumentCount(verifyDb);
            REQUIRE(count == docIDs.size());
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
    static std::unordered_map<alloc_slice, unsigned> getDocIDs(C4Database* db) {
        std::unordered_map<alloc_slice, unsigned> ret;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, ERROR_INFO());
        while (c4enum_next(e, ERROR_INFO())) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            ret.emplace(info.docID, c4rev_getGeneration(info.revID));
        }
        return ret;
    }

    /*
     * Set up test parameters (testUser, user auth).
     * Sets up the _sg.authHeader for user authorisation for `SG` class REST requests.
     * Easiest usage of the function is i.e. `initTest()` which will create the SG user with access to
     * all channels, with username == kTestUserName.
     * Once called, test parameters can be access by; _testUser and _docIDs.
     * If you're using a channel filter, you can do i.e. `initTest({ channel1, channel2 })`
     */
    void initTest(const std::vector<std::string>& channelIDs = {"*"},
                  const std::string& username = kTestUserName) {
        // Avoid copy constructor
        new (&_testUser) SG::TestUser { _sg, username, channelIDs };
        _options = createOptionsAuth(_testUser._username, _testUser._password);
        _sg.authHeader = _testUser.authHeader();
    }

    /*
     * Fetch the docIDs from the db and update _docIDs with the result.
     * If you're using a docID filter, you can call:
     *   updateDocIDs();
     *   replParams.setDocIDs(_docIDs);
     * to fetch the latest docIDs and update the docID filter on your ReplParams
     */
    void updateDocIDs() {
        _docIDs = getDocIDs(db);
    }

    struct CipherContext {
        slice docID;
        slice keyPath;
        bool called;

        CipherContext(const char* id, const char* path, bool called_)
                : docID(id)
                , keyPath(path)
                , called(called_)
        {}
    };

    std::unique_ptr<CipherContext> encContext;
    std::unique_ptr<CipherContext> decContext;

    SG::TestUser _testUser {};
    std::unordered_map<alloc_slice, unsigned> _docIDs {};
};