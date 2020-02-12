//
// c4DatabaseEncryptionTest.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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

    void checkBadKey(const C4DatabaseConfig &config) {
        assert(!db);
        C4Error error;
        db = c4db_open(databasePath(), &config, &error);
        CHECK(!db);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotADatabaseFile);
    }
};


TEST_CASE("Database Key Derivation", "[Database][Encryption][C]") {
    C4EncryptionKey key = {};
    REQUIRE(!c4key_setPassword(&key, nullslice, kC4EncryptionAES256));
    REQUIRE(!c4key_setPassword(&key, "password123"_sl, kC4EncryptionNone));

    REQUIRE(c4key_setPassword(&key, "password123"_sl, kC4EncryptionAES256));
    CHECK(key.algorithm == kC4EncryptionAES256);
    CHECK(slice(key.bytes, sizeof(key.bytes)).hexString() ==
          "ad3470ce03363552b20a4a70a4aec02cb7439f6202e75b231ab57f2d5e716909");
}

N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Wrong Key", "[Database][Encryption][C]") {
    createNumberedDocs(99);

    C4DatabaseConfig config = *c4db_getConfig(db), badConfig = config;
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
    db = c4db_open(databasePath(), &config, &error);
    REQUIRE(db);
    CHECK(c4db_getDocumentCount(db) == 99);
}


N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Rekey", "[Database][Encryption][blob][C]") {
    createNumberedDocs(99);

    // Add blob to the store:
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");
    C4BlobKey blobKey;
    C4Error error;
    auto blobStore = c4db_getBlobStore(db, &error);
    REQUIRE(blobStore);
    REQUIRE(c4blob_create(blobStore, blobToStore, nullptr, &blobKey, &error));

    C4SliceResult blobResult = c4blob_getContents(blobStore, blobKey, &error);
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // If we're on the unencrypted pass, encrypt the db. Otherwise decrypt it:
    C4EncryptionKey newKey = {kC4EncryptionNone, {}};
    if (c4db_getConfig(db)->encryptionKey.algorithm == kC4EncryptionNone) {
        newKey.algorithm = kC4EncryptionAES256;
        memcpy(newKey.bytes, "a different key than default....", kC4EncryptionKeySizeAES256);
        REQUIRE(c4db_rekey(db, &newKey, &error));
    } else {
        REQUIRE(c4db_rekey(db, nullptr, &error));
    }

    // Verify the db works:
    REQUIRE(c4db_getDocumentCount(db) == 99);
    REQUIRE(blobStore);
    blobResult = c4blob_getContents(blobStore, blobKey, &error);
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // Check that db can be reopened with the new key:
    REQUIRE(c4db_getConfig(db)->encryptionKey.algorithm == newKey.algorithm);
    REQUIRE(memcmp(c4db_getConfig(db)->encryptionKey.bytes, newKey.bytes, 32) == 0);
    reopenDB();
}


static void testOpeningEncryptedDBFixture(const char *dbPath, const void *key) {
    static const C4DatabaseFlags kFlagsToTry[] = {kC4DB_ReadOnly, /*kC4DB_NoUpgrade,*/ 0};
    // Skipping NoUpgrade because schema version 302 is mandatory for writeable dbs in CBL 2.7.

    for (C4DatabaseFlags flag : kFlagsToTry) {
        C4DatabaseConfig config = { };
        config.flags = flag;
        config.encryptionKey.algorithm = kC4EncryptionAES256;
        memcpy(config.encryptionKey.bytes, key, kC4EncryptionKeySizeAES256);
        C4Error error;
        C4Log("---- Opening db %s with flags 0x%x", dbPath, config.flags);
        auto db = c4db_open(C4Test::copyFixtureDB(dbPath), &config, &error);
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
