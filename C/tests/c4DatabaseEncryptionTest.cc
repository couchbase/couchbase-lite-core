//
// c4DatabaseEncryptionTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4BlobStore.h"
#include "FilePath.hh"
#include <cmath>
#include <errno.h>
#include <iostream>

using namespace std;
using FilePath = litecore::FilePath;


#ifdef COUCHBASE_ENTERPRISE


class C4EncryptionTest : public C4Test {
public:
    C4EncryptionTest(int testOption) :C4Test(testOption) { }

    void checkBadKey(const C4DatabaseConfig2 &config) {
        assert(!db);
        C4Error error;
        db = c4db_openNamed(kDatabaseName, &config, &error);
        CHECK(!db);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotADatabaseFile);
    }
};


TEST_CASE("Database Key Derivation", "[Database][Encryption][C]") {
    bool (*c4key_setPasswordFunc)(C4EncryptionKey *encryptionKey,
                                  C4String password,
                                  C4EncryptionAlgorithm alg) = nullptr;
    string expectedKey;
    C4EncryptionKey key {};
    SECTION("SHA256") {
        c4key_setPasswordFunc = c4key_setPassword;
        expectedKey = "ad3470ce03363552b20a4a70a4aec02cb7439f6202e75b231ab57f2d5e716909";
    }
    SECTION("SHA1") {
        c4key_setPasswordFunc = c4key_setPasswordSHA1;
        expectedKey = "7ecec9cc8d4efbebcbf537a3169f61d9db05971a9fec9761ff37fdb1f09f862d";
    }
    {
        ExpectingExceptions expectingExceptions;
        REQUIRE(!(*c4key_setPasswordFunc)(&key, nullslice, kC4EncryptionAES256));
        REQUIRE(!(*c4key_setPasswordFunc)(&key, "password123"_sl, kC4EncryptionNone));
    }
    key = {};
    REQUIRE((*c4key_setPasswordFunc)(&key, "password123"_sl, kC4EncryptionAES256));
    CHECK(key.algorithm == kC4EncryptionAES256);
    CHECK(slice(key.bytes, sizeof(key.bytes)).hexString() == expectedKey);
}

N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Wrong Key", "[Database][Encryption][C]") {
    createNumberedDocs(99);

    C4DatabaseConfig2 config = dbConfig(), badConfig = config;
    closeDB();

    C4Error error;
    if (config.encryptionKey.algorithm == kC4EncryptionNone) {
        // DB is not encrypted; try using a key:
        badConfig.encryptionKey.algorithm = kC4EncryptionAES256;
        memset(badConfig.encryptionKey.bytes, 0x7F, sizeof(badConfig.encryptionKey.bytes));
        ExpectingExceptions x;
        checkBadKey(badConfig);
    } else {
        // DB is encrypted. Try giving the wrong key:
        badConfig.encryptionKey.bytes[9] ^= 0xFF;
        ExpectingExceptions x;
        checkBadKey(badConfig);
        // Try giving no key:
        badConfig.encryptionKey.algorithm = kC4EncryptionNone;
        checkBadKey(badConfig);
    }

    // Reopen with correct key:
    db = c4db_openNamed(kDatabaseName, &config, ERROR_INFO(error));
    REQUIRE(db);
    CHECK(c4db_getDocumentCount(db) == 99);
}


N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Rekey", "[Database][Encryption][blob][C]") {
    createNumberedDocs(99);

    // Add blob to the store:
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");
    C4BlobKey blobKey;
    C4Error error;
    auto blobStore = c4db_getBlobStore(db, ERROR_INFO(error));
    REQUIRE(blobStore);
    REQUIRE(c4blob_create(blobStore, blobToStore, nullptr, &blobKey, WITH_ERROR(&error)));

    C4SliceResult blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // If we're on the unencrypted pass, encrypt the db. Otherwise decrypt it:
    C4EncryptionKey newKey = {kC4EncryptionNone, {}};
    if (c4db_getConfig2(db)->encryptionKey.algorithm == kC4EncryptionNone) {
        newKey.algorithm = kC4EncryptionAES256;
        memcpy(newKey.bytes, "a different key than default....", kC4EncryptionKeySizeAES256);
        REQUIRE(c4db_rekey(db, &newKey, WITH_ERROR(&error)));
    } else {
        REQUIRE(c4db_rekey(db, nullptr, WITH_ERROR(&error)));
    }

    // Verify the db works:
    REQUIRE(c4db_getDocumentCount(db) == 99);
    REQUIRE(blobStore);
    blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // Check that db can be reopened with the new key:
    REQUIRE(c4db_getConfig2(db)->encryptionKey.algorithm == newKey.algorithm);
    REQUIRE(memcmp(c4db_getConfig2(db)->encryptionKey.bytes, newKey.bytes, 32) == 0);
    reopenDB();
}


static void testOpeningEncryptedDBFixture(const char *dbPath, const void *key) {
    static const C4DatabaseFlags kFlagsToTry[] = {/*kC4DB_ReadOnly, kC4DB_NoUpgrade,*/ 0};
    // Skipping NoUpgrade because schema version 302 is mandatory for writeable dbs in CBL 2.7.
    // Skipping ReadOnly because CBL 3.0 can't open 2.x dbs without upgrading them.

    for (C4DatabaseFlags flag : kFlagsToTry) {
        C4DatabaseConfig2 config = { };
        config.parentDirectory = slice(TempDir());
        config.flags = flag;
        config.encryptionKey.algorithm = kC4EncryptionAES256;
        memcpy(config.encryptionKey.bytes, key, kC4EncryptionKeySizeAES256);
        C4Error error;
        C4Log("---- Opening db %s with flags 0x%x", dbPath, config.flags);
        auto db = c4db_openNamed(C4Test::copyFixtureDB(dbPath), &config, ERROR_INFO(error));
        CHECK(db);
        c4db_release(db);
    }
}


TEST_CASE("Database Open Older Encrypted", "[Database][Encryption][C]") {
    testOpeningEncryptedDBFixture("encrypted_databases/Mac_2.5_AES256.cblite2",
                                  "a different key than default....");
}


#ifdef __APPLE__

TEST_CASE("Database Upgrade AES128", "[Database][Encryption][C]") {
    C4EncryptionKey key;
    c4key_setPassword(&key, "password123"_sl, kC4EncryptionAES256);
    testOpeningEncryptedDBFixture("encrypted_databases/Mac_2.1_AES128.cblite2", key.bytes);
}

#endif // __APPLE__

#endif // COUCHBASE_ENTERPRISE
