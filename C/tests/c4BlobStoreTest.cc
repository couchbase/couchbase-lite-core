//
// c4BlobStoreTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "c4BlobStore.h"
#include "c4Private.h"
#include <fstream>

using namespace std;

class BlobStoreTest : public C4Test {
  public:
    BlobStoreTest(int option) : C4Test(option), encrypted(isEncrypted()), store(c4db_getBlobStore(db, nullptr)) {}

    C4BlobStore* store{nullptr};
    const bool   encrypted;

    C4BlobKey bogusKey;
};

TEST_CASE("parse blob keys", "[blob][C]") {
    C4BlobKey key1;
    memset(key1.bytes, 0x55, sizeof(key1.bytes));
    alloc_slice str = c4blob_keyToString(key1);
    CHECK(string((char*)str.buf, str.size) == "sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=");

    C4BlobKey key2;
    CHECK(c4blob_keyFromString(C4Slice{str.buf, str.size}, &key2));
    CHECK(memcmp(key1.bytes, key2.bytes, sizeof(key1.bytes)) == 0);
}

TEST_CASE("parse invalid blob keys", "[blob][C][!throws]") {
    ExpectingExceptions x;
    C4BlobKey           key2;
    CHECK_FALSE(c4blob_keyFromString(C4STR(""), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("rot13-xxxx"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-VVVVVVVVVVVVVVVVVVVVVV"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVU"), &key2));
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "missing blobs", "[blob][C]") {
    ExpectingExceptions x;

    CHECK(c4blob_getSize(store, bogusKey) == -1);

    C4Error       error = {};
    C4SliceResult data  = c4blob_getContents(store, bogusKey, &error);
    CHECK(data.buf == nullptr);
    CHECK(data.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
    error = {};
    data  = c4blob_getFilePath(store, bogusKey, &error);
    CHECK(data.buf == nullptr);
    CHECK(data.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "create blobs", "[blob][Encryption][C]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");

    // Add blob to the store:
    C4BlobKey key;
    C4Error   error;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key, WITH_ERROR(&error)));

    alloc_slice str = c4blob_keyToString(key);
    CHECK(string((char*)str.buf, str.size) == "sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E=");
    CHECK(memcmp(c4blob_computeKey(blobToStore).bytes, key.bytes, 20) == 0);

    // Read it back and compare
    int64_t blobSize = c4blob_getSize(store, key);
    CHECK(blobSize >= blobToStore.size);
    if ( encrypted ) CHECK(blobSize <= blobToStore.size + 16);  // getSize is approximate in an encrypted store
    else
        CHECK(blobSize == blobToStore.size);

    alloc_slice gotBlob = c4blob_getContents(store, key, ERROR_INFO(error));
    REQUIRE(gotBlob.buf != nullptr);
    REQUIRE(gotBlob.size == blobToStore.size);
    CHECK(memcmp(gotBlob.buf, blobToStore.buf, gotBlob.size) == 0);

    if ( encrypted ) {
        ExpectingExceptions x;
        alloc_slice         p = c4blob_getFilePath(store, key, &error);
        CHECK(!p);
        CHECK(error.code == kC4ErrorWrongFormat);
    } else {
        alloc_slice p = c4blob_getFilePath(store, key, &error);
        REQUIRE(p);
        string path((char*)p.buf, p.size);
        string filename = "QneWo5IYIQ0ZrbCG0hXPGC6jy7E=.blob";
        CHECK(path.find(filename) == path.size() - filename.size());
    }

    // Try storing it again
    C4BlobKey key2;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key2, WITH_ERROR(&error)));
    CHECK(memcmp(&key2, &key, sizeof(key2)) == 0);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "delete blobs", "[blob][Encryption][C]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");

    // Add blob to the store:
    C4BlobKey key;
    C4Error   error;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key, WITH_ERROR(&error)));

    alloc_slice str = c4blob_keyToString(key);
    CHECK(string((char*)str.buf, str.size) == "sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E=");

    // Delete it
    REQUIRE(c4blob_delete(store, key, WITH_ERROR(&error)));

    // Try to read it (should be gone):
    int64_t blobSize = c4blob_getSize(store, key);
    CHECK(blobSize == -1);

    {
        ExpectingExceptions x;
        auto                gotBlob = c4blob_getContents(store, key, &error);
        REQUIRE(gotBlob.buf == nullptr);
        REQUIRE(gotBlob.size == 0);
    }

    C4SliceResult p = c4blob_getFilePath(store, key, &error);
    CHECK(p.buf == nullptr);
    CHECK(p.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
    c4slice_free(p);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "create blob, key mismatch", "[blob][Encryption][C][!throws]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");

    // Add blob to the store but give an expectedKey that doesn't match:
    C4BlobKey key, expectedKey;
    memset(&expectedKey, 0x55, sizeof(expectedKey));
    C4Error error;
    {
        ExpectingExceptions x;
        CHECK(!c4blob_create(store, blobToStore, &expectedKey, &key, &error));
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorCorruptData);
    }

    // Try again but give the correct expectedKey:
    c4blob_keyFromString(C4STR("sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E="), &expectedKey);
    CHECK(c4blob_create(store, blobToStore, &expectedKey, &key, WITH_ERROR(&error)));
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "read blob with stream", "[blob][Encryption][C]") {
    string blob = "This is a blob to store in the store!";

    // Add blob to the store:
    C4BlobKey key;
    C4Error   error;
    REQUIRE(c4blob_create(store, {blob.data(), blob.size()}, nullptr, &key, WITH_ERROR(&error)));

    {
        ExpectingExceptions x;
        CHECK(c4blob_openReadStream(store, bogusKey, &error) == nullptr);
        CHECK(error == C4Error{LiteCoreDomain, kC4ErrorNotFound});
    }

    char   buf[10000];
    size_t kReadSizes[5] = {1, 6, blob.size(), 4096, 10000};
    for ( int i = 0; i < 5; i++ ) {
        size_t readSize = kReadSizes[i];

        auto stream = c4blob_openReadStream(store, key, ERROR_INFO(error));
        REQUIRE(stream);

        // Read it back, 6 bytes at a time:
        string readBack;
        size_t bytesRead;
        do {
            bytesRead = c4stream_read(stream, buf, readSize, ERROR_INFO(error));
            readBack.append(buf, bytesRead);
        } while ( bytesRead == readSize );
        REQUIRE(error.code == 0);
        CHECK(readBack == blob);

        // Try seeking:
        REQUIRE(c4stream_seek(stream, 10, WITH_ERROR(&error)));
        REQUIRE(c4stream_read(stream, buf, 4, WITH_ERROR(&error)) == 4);
        CHECK(memcmp(buf, "blob", 4) == 0);

        CHECK(c4stream_getLength(stream, WITH_ERROR(&error)) == blob.size());

        c4stream_close(stream);
        c4stream_close(nullptr);  // this should be a no-op, not a crash
    }
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blob with stream", "[blob][Encryption][C]") {
    // Write the blob:
    C4Error        error;
    C4WriteStream* stream = c4blob_openWriteStream(store, ERROR_INFO(error));
    REQUIRE(stream);
    CHECK(c4stream_bytesWritten(stream) == 0);

    constexpr size_t bufSize = 100, readBufSize = 100;

    for ( int i = 0; i < 1000; i++ ) {
        char buf[bufSize];
        snprintf(buf, bufSize, "This is line %03d.\n", i);
        REQUIRE(c4stream_write(stream, buf, strlen(buf), WITH_ERROR(&error)));
    }

    CHECK(c4stream_bytesWritten(stream) == 18 * 1000);

    // Get the blob key, and install it:
    C4BlobKey key = c4stream_computeBlobKey(stream);
    CHECK(c4stream_install(stream, nullptr, WITH_ERROR(&error)));
    c4stream_closeWriter(stream);
    c4stream_closeWriter(nullptr);

    C4SliceResult keyStr = c4blob_keyToString(key);
    CHECK(string((char*)keyStr.buf, keyStr.size) == "sha1-0htkjBHcrTyIk9K8e1zZq47yWxw=");
    c4slice_free(keyStr);

    // Read it back using the key:
    C4SliceResult contents = c4blob_getContents(store, key, ERROR_INFO(error));
    CHECK(contents.size == 18 * 1000);
    c4slice_free(contents);

    // Read it back random-access:
    C4ReadStream* reader = c4blob_openReadStream(store, key, ERROR_INFO(error));
    REQUIRE(reader);
    static const int increment = 3 * 3 * 3 * 3;
    int              line      = increment;
    for ( uint64_t i = 0; i < 1000; i++ ) {
        line = (line + increment) % 1000;
        INFO("Reading line " << line << " at offset " << 18 * line);
        char buf[bufSize], readBuf[readBufSize];
        snprintf(buf, bufSize, "This is line %03d.\n", line);
        REQUIRE(c4stream_seek(reader, 18 * line, WITH_ERROR(&error)));
        REQUIRE(c4stream_read(reader, readBuf, 18, WITH_ERROR(&error)) == 18);
        readBuf[18] = '\0';
        REQUIRE(string(readBuf) == string(buf));
    }
    c4stream_close(reader);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blobs of many sizes", "[blob][Encryption][C]") {
    // The interesting sizes for encrypted blobs are right around the file block size (4096)
    // and the cipher block size (16).
    const vector<size_t> kSizes
            = {0, 1, 15, 16, 17, 4095, 4096, 4097, 4096 + 15, 4096 + 16, 4096 + 17, 8191, 8192, 8193};
    for ( size_t size : kSizes ) {
        //Log("---- %lu-byte blob", size);
        INFO("Testing " << size << "-byte blob");
        // Write the blob:
        C4Error        error;
        C4WriteStream* stream = c4blob_openWriteStream(store, ERROR_INFO(error));
        REQUIRE(stream);

        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXY";
        for ( int i = 0; i < size; i++ ) {
            int c = i % strlen(chars);
            REQUIRE(c4stream_write(stream, &chars[c], 1, WITH_ERROR(&error)));
        }

        // Get the blob key, and install it:
        C4BlobKey key = c4stream_computeBlobKey(stream);
        CHECK(c4stream_install(stream, nullptr, WITH_ERROR(&error)));
        c4stream_closeWriter(stream);

        // Read it back using the key:
        C4SliceResult contents = c4blob_getContents(store, key, ERROR_INFO(error));
        CHECK(contents.size == size);
        for ( int i = 0; i < size; i++ ) CHECK(((const char*)contents.buf)[i] == chars[i % strlen(chars)]);
        c4slice_free(contents);
    }
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blob and cancel", "[blob][Encryption][C]") {
    // Write the blob:
    C4Error        error;
    C4WriteStream* stream = c4blob_openWriteStream(store, ERROR_INFO(error));
    REQUIRE(stream);

    const char* buf = "This is line oops\n";
    CHECK(c4stream_write(stream, buf, strlen(buf), WITH_ERROR(&error)));

    c4stream_closeWriter(stream);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write identical blob", "[blob][C]") {
    // CBL-670: Installing identical blob can cause filesystem issues on Windows,
    // but this is hard to reproduce, so simulate having the file open (which is what
    // somehow ends up happening)

    if ( encrypted ) {
        // Can't get file paths with encryption, and can't compile BlobStoreTest with
        // TEST_CASE_METHOD macro
        return;
    }

    C4Error        error;
    const int      streamCount = 2;
    C4WriteStream* streams[streamCount];
    for ( int iter = 0; iter < streamCount; iter++ ) {
        C4WriteStream* stream = c4blob_openWriteStream(store, ERROR_INFO(error));
        REQUIRE(stream);
        CHECK(c4stream_bytesWritten(stream) == 0);
        streams[iter]            = stream;
        constexpr size_t bufSize = 100;

        for ( int i = 0; i < 1000; i++ ) {
            char buf[bufSize];
            snprintf(buf, bufSize, "This is line %03d.\n", i);
            REQUIRE(c4stream_write(stream, buf, strlen(buf), WITH_ERROR(&error)));
        }

        CHECK(c4stream_bytesWritten(stream) == 18 * 1000);
        C4BlobKey key = c4stream_computeBlobKey(stream);
        if ( iter > 0 ) {
            alloc_slice path = c4blob_getFilePath(store, key, ERROR_INFO(error));
            REQUIRE(path.buf != nullptr);
            auto pathStr = string((char*)path.buf, path.size);

            // Simulate the file being in use
            ifstream fin(pathStr);
            CHECK(c4stream_install(stream, nullptr, WITH_ERROR(&error)));
        } else {
            CHECK(c4stream_install(stream, nullptr, WITH_ERROR(&error)));
        }

        C4SliceResult keyStr = c4blob_keyToString(key);
        CHECK(string((char*)keyStr.buf, keyStr.size) == "sha1-0htkjBHcrTyIk9K8e1zZq47yWxw=");
        c4slice_free(keyStr);

        // Read it back using the key:
        C4SliceResult contents = c4blob_getContents(store, key, ERROR_INFO(error));
        CHECK(contents.size == 18 * 1000);
        c4slice_free(contents);
    }

    for ( auto& stream : streams ) { c4stream_closeWriter(stream); }
}
