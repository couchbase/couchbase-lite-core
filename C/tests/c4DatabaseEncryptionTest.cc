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

#include "c4Test.hh"  // IWYU pragma: keep
#include "c4BlobStore.h"
#include "FilePath.hh"
#include "c4Collection.h"
#include <cmath>
#include <cerrno>
#include <iostream>

using namespace std;
using FilePath = litecore::FilePath;


#ifdef COUCHBASE_ENTERPRISE


class C4EncryptionTest : public C4Test {
  public:
    explicit C4EncryptionTest(int testOption) : C4Test(testOption) {}

    void checkBadKey(const C4DatabaseConfig2& config) {
        assert(!db);
        C4Error error;
        db = c4db_openNamed(kDatabaseName, &config, &error);
        CHECK(!db);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotADatabaseFile);
    }
};

TEST_CASE("Database Key Derivation", "[Database][Encryption][C]") {
    bool (*c4key_setPasswordFunc)(C4EncryptionKey* encryptionKey, C4String password, C4EncryptionAlgorithm alg) =
            nullptr;
    string          expectedKey;
    C4EncryptionKey key{};
    SECTION("SHA256") {
        c4key_setPasswordFunc = c4key_setPassword;
        expectedKey           = "ad3470ce03363552b20a4a70a4aec02cb7439f6202e75b231ab57f2d5e716909";
    }
    SECTION("SHA1") {
        c4key_setPasswordFunc = c4key_setPasswordSHA1;
        expectedKey           = "7ecec9cc8d4efbebcbf537a3169f61d9db05971a9fec9761ff37fdb1f09f862d";
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
    if ( config.encryptionKey.algorithm == kC4EncryptionNone ) {
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
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    CHECK(c4coll_getDocumentCount(defaultColl) == 99);
}

N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Rekey", "[Database][Encryption][blob][C]") {
    createNumberedDocs(99);

    // Add blob to the store:
    C4Slice   blobToStore = C4STR("This is a blob to store in the store!");
    C4BlobKey blobKey;
    C4Error   error;
    auto      blobStore = c4db_getBlobStore(db, ERROR_INFO(error));
    REQUIRE(blobStore);
    REQUIRE(c4blob_create(blobStore, blobToStore, nullptr, &blobKey, WITH_ERROR(&error)));

    C4SliceResult blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // If we're on the unencrypted pass, encrypt the db. Otherwise decrypt it:
    C4EncryptionKey newKey = {kC4EncryptionNone, {}};
    if ( c4db_getConfig2(db)->encryptionKey.algorithm == kC4EncryptionNone ) {
        newKey.algorithm = kC4EncryptionAES256;
        memcpy(newKey.bytes, "a different key than default....", kC4EncryptionKeySizeAES256);
        REQUIRE(c4db_rekey(db, &newKey, WITH_ERROR(&error)));
    } else {
        REQUIRE(c4db_rekey(db, nullptr, WITH_ERROR(&error)));
    }

    // Verify the db works:
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 99);
    REQUIRE(blobStore);
    blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);
    c4slice_free(blobResult);

    // Check that db can be reopened with the new key:
    REQUIRE(c4db_getConfig2(db)->encryptionKey.algorithm == newKey.algorithm);
    REQUIRE(memcmp(c4db_getConfig2(db)->encryptionKey.bytes, newKey.bytes, 32) == 0);
    reopenDB();
}

N_WAY_TEST_CASE_METHOD(C4EncryptionTest, "Database Rekey Atomicity", "[Database][Encryption][blob][C]") {
    // Rekeying between "no key" and a key is covered by "Database Rekey". Recovery treats
    // "no key" like any other key, so this test only needs the encrypted variant.
    if ( c4db_getConfig2(db)->encryptionKey.algorithm == kC4EncryptionNone ) return;

    C4DatabaseConfig2 dbConfig = *c4db_getConfig2(db);
    FilePath          parentDir{slice(dbConfig.parentDirectory)};
    dbConfig.parentDirectory = slice(parentDir.dirName());
    FilePath dbPath{slice(c4db_getPath(db))};

    C4EncryptionKey key0   = dbConfig.encryptionKey;
    C4EncryptionKey newKey = key0;
    memcpy(newKey.bytes, "a different key than default....", kC4EncryptionKeySizeAES256);

    // A temporary directory that holds the database snapshots.
    static constexpr const char* kRekeyingDirName   = "Attachments_rekeying";
    static constexpr const char* kStagedBlobDirName = "Attachments_staged";
    FilePath                     tempDir            = parentDir.subdirectoryNamed(".temp");
    // The cleanup at the end of this test is skipped when an earlier run fails (for example,
    // an exception or a failed REQUIRE), which leaves the snapshots behind. Remove them now:
    // blobs are read-only files, which cannot be overwritten when copying on Linux.
    if ( tempDir.exists() ) tempDir.delRecursive();
    tempDir.mkdir();
    auto key0Snapshot   = tempDir.subdirectoryNamed("key0");
    auto newKeySnapshot = tempDir.subdirectoryNamed("newKey");

    createNumberedDocs(99);

    // Add a blob to the store, encrypted with the original key.
    C4Slice   blobToStore = C4STR("This is a blob to store in the store!");
    C4BlobKey blobKey;
    C4Error   error;
    auto      blobStore = c4db_getBlobStore(db, ERROR_INFO(error));
    REQUIRE(blobStore);
    REQUIRE(c4blob_create(blobStore, blobToStore, nullptr, &blobKey, WITH_ERROR(&error)));

    alloc_slice blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);

    // Snapshot the database bundle while it is still on key0.
    parentDir.subdirectoryNamed(dbPath.fileOrDirName()).copyTo(key0Snapshot);

    // Rekey from key0 to newKey.
    REQUIRE(c4db_rekey(db, &newKey, WITH_ERROR(&error)));

    // A successful rekey leaves the database and Attachments encrypted with newKey. It leaves
    // neither the scratch directory (kRekeyingDirName) nor the staged directory
    // (kStagedBlobDirName) behind.
    CHECK(!dbPath[kRekeyingDirName].exists());
    CHECK(!dbPath[kStagedBlobDirName].exists());

    // Verify that the database is now on newKey and still works.
    REQUIRE(c4db_getConfig2(db)->encryptionKey.algorithm == newKey.algorithm);
    REQUIRE(memcmp(c4db_getConfig2(db)->encryptionKey.bytes, newKey.bytes, 32) == 0);
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 99);
    blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
    CHECK(blobResult == blobToStore);

    // Snapshot the database bundle again, now on newKey.
    parentDir.subdirectoryNamed(dbPath.fileOrDirName()).copyTo(newKeySnapshot);

    // Reopening with newKey works.
    reopenDB();

    closeDB();

    // The database is now on disk with newKey.
    // Each section below sets up the on-disk state that a crash during rekey() would leave.
    // After each of them, the database must open with exactly one of the two keys (the one it
    // is really on) and its attachments must be readable, unless the open is expected to fail.

    C4EncryptionKey *onKey = nullptr, *otherKey = nullptr;
    bool             readOnlyFailure  = false;
    bool             stagedRemains    = false;  // kStagedBlobDirName still exists after the open
    bool             rekeyingLeftover = false;  // kRekeyingDirName still exists after the open
    bool             rekeyAfterOpen   = false;  // after a writeable open, rekey again and check the cleanup
    dbConfig.flags                    = dbConfig.flags & ~kC4DB_Create;

    // Precondition: the disk holds the result of the successful rekey above, so both
    // db.sqlite3 and Attachments are encrypted with newKey.

    SECTION("db rekeyed, attachments interrupted") {
        // The process died after the SQLite database was rekeyed, but before the staged
        // attachments were installed. On disk, db.sqlite3 is on newKey and Attachments is still
        // on key0. The staged directory holds the attachments encrypted with newKey.
        onKey    = &newKey;
        otherKey = &key0;
        dbPath["Attachments"].moveTo(dbPath[kStagedBlobDirName]);
        // Restore the old Attachments, still encrypted with key0.
        // (Directories are copied through subdirectoryNamed(), whose trailing separator marks the
        // path as a directory; copyTo() treats other paths as plain files on Linux.)
        key0Snapshot.subdirectoryNamed("Attachments").copyTo(dbPath.subdirectoryNamed("Attachments"));

        SECTION("Read-only Open") {
            // A read-only open must fail: it would have to install the staged Attachments,
            // which modifies files.
            // Expected warning in the log: Staged attachments store 'Attachments_staged' must be
            // installed before this database can be used, but it is open read-only
            dbConfig.flags  = dbConfig.flags | kC4DB_ReadOnly;
            readOnlyFailure = true;
            stagedRemains   = true;  // untouched: a read-only open must not modify files
        }

        SECTION("Writable Open") { REQUIRE((dbConfig.flags & kC4DB_ReadOnly) == 0); }
    }

    SECTION("db rekey failed, staged not cleared") {
        // The rekey failed or the process died before the SQLite rekey committed. The staged
        // directory is not removed (rekey() leaves it for the next open to settle). db.sqlite3 is
        // still on key0, and the staged directory holds Attachments encrypted with newKey.
        onKey    = &key0;
        otherKey = &newKey;
        dbPath.delRecursive();
        key0Snapshot.copyTo(dbPath);
        newKeySnapshot.subdirectoryNamed("Attachments").copyTo(dbPath.subdirectoryNamed(kStagedBlobDirName));

        SECTION("Read-only Open") {
            // A read-only open works: the leftover directory is ignored, not deleted.
            // Expected info message in the log: Ignoring leftover staged attachments store
            // 'Attachments_staged' (read-only)
            dbConfig.flags = dbConfig.flags | kC4DB_ReadOnly;
            stagedRemains  = true;  // ignored, not deleted
        }

        SECTION("Writable Open") { REQUIRE((dbConfig.flags & kC4DB_ReadOnly) == 0); }
    }

    SECTION("db rekey failed, staged is cleared") {
        // Nothing is left over, for example because the process died before the rename. The disk
        // is in its state from before the rekey: db.sqlite3 and Attachments are both on key0.
        onKey    = &key0;
        otherKey = &newKey;
        dbPath.delRecursive();
        key0Snapshot.copyTo(dbPath);

        SECTION("Read-only Open") { dbConfig.flags = dbConfig.flags | kC4DB_ReadOnly; }

        SECTION("Writable Open") { REQUIRE((dbConfig.flags & kC4DB_ReadOnly) == 0); }
    }

    SECTION("crashed while copying attachments") {
        // The process died while the rekeyed attachments were being copied into the scratch
        // directory. Nothing was staged and the database is still on key0. Opening ignores the
        // scratch directory, and the next rekey clears it.
        onKey    = &key0;
        otherKey = &newKey;
        dbPath.delRecursive();
        key0Snapshot.copyTo(dbPath);
        newKeySnapshot.subdirectoryNamed("Attachments").copyTo(dbPath.subdirectoryNamed(kRekeyingDirName));
        rekeyingLeftover = true;

        SECTION("Read-only Open") { dbConfig.flags = dbConfig.flags | kC4DB_ReadOnly; }

        SECTION("Writable Open") {
            REQUIRE((dbConfig.flags & kC4DB_ReadOnly) == 0);
            rekeyAfterOpen = true;
        }
    }

    // Opening with the key that the database is not on must fail, whatever the state of the
    // attachments.
    C4Error c4error{};
    dbConfig.encryptionKey = *otherKey;
    db                     = c4db_openNamed(kDatabaseName, &dbConfig, &c4error);
    CHECK(c4error.domain == LiteCoreDomain);
    CHECK(c4error.code == kC4ErrorNotADatabaseFile);
    CHECK(!db);
    if ( db ) closeDB();

    dbConfig.encryptionKey = *onKey;
    db                     = c4db_openNamed(kDatabaseName, &dbConfig, &c4error);
    if ( readOnlyFailure ) {
        CHECK(c4error.domain == LiteCoreDomain);
        CHECK(c4error.code == kC4ErrorNotWriteable);
        CHECK(!db);
    } else {
        REQUIRE(db);
        blobStore  = c4db_getBlobStore(db, ERROR_INFO(error));
        blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
        CHECK(blobResult == blobToStore);
    }

    // Check what the open did to the leftover directories.
    CHECK(dbPath[kStagedBlobDirName].exists() == stagedRemains);
    CHECK(dbPath[kRekeyingDirName].exists() == rekeyingLeftover);

    if ( rekeyAfterOpen ) {
        // A later rekey must succeed and clear the leftover scratch directory.
        REQUIRE(c4db_rekey(db, otherKey, WITH_ERROR(&error)));
        CHECK(!dbPath[kRekeyingDirName].exists());
        CHECK(!dbPath[kStagedBlobDirName].exists());
        blobResult = c4blob_getContents(blobStore, blobKey, ERROR_INFO(error));
        CHECK(blobResult == blobToStore);
    }

    tempDir.delRecursive();
}

static void testOpeningEncryptedDBFixture(const char* dbPath, const void* key) {
    static const C4DatabaseFlags kFlagsToTry[] = {/*kC4DB_ReadOnly, kC4DB_NoUpgrade,*/ 0};
    // Skipping NoUpgrade because schema version 302 is mandatory for writeable dbs in CBL 2.7.
    // Skipping ReadOnly because CBL 3.0 can't open 2.x dbs without upgrading them.

    for ( C4DatabaseFlags flag : kFlagsToTry ) {
        C4DatabaseConfig2 config       = {};
        config.parentDirectory         = slice(TempDir());
        config.flags                   = flag;
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
    testOpeningEncryptedDBFixture("encrypted_databases/Mac_2.5_AES256.cblite2", "a different key than default....");
}


#    ifdef __APPLE__

TEST_CASE("Database Upgrade AES128", "[Database][Encryption][C]") {
    C4EncryptionKey key;
    REQUIRE(c4key_setPassword(&key, "password123"_sl, kC4EncryptionAES256));
    testOpeningEncryptedDBFixture("encrypted_databases/Mac_2.1_AES128.cblite2", key.bytes);
}

#    endif  // __APPLE__

#endif  // COUCHBASE_ENTERPRISE
