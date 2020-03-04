//
// c4BlobStoreTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "c4BlobStore.h"
#include "c4Private.h"
#include <fstream>

using namespace std;


class BlobStoreTest {
public:
    
    static const int numberOfOptions = 2;       // 0 = unencrypted, 1 = encrypted

    BlobStoreTest(int option)
    :encrypted(option == 1)
    {
        C4EncryptionKey crypto, *encryption=nullptr;
        if (encrypted) {
            fprintf(stderr, "        ...encrypted\n");
            INFO("(Encrypted)");
            crypto.algorithm = kC4EncryptionAES256;
            memset(&crypto.bytes, 0xCC, sizeof(crypto.bytes));
            encryption = &crypto;
        }

        C4Error error;
        store = c4blob_openStore(TEMPDIR("cbl_blob_test" + kPathSeparator),
                                 kC4DB_Create,
                                 encryption,
                                 &error);
        REQUIRE(store != nullptr);

        memset(bogusKey.bytes, 0x55, sizeof(bogusKey.bytes));
    }

    ~BlobStoreTest() {
        C4Error error;
        CHECK(c4blob_deleteStore(store, &error));
    }

    C4BlobStore *store {nullptr};
    const bool encrypted;

    C4BlobKey bogusKey;
};


TEST_CASE("parse blob keys", "[blob][C]") {
    C4BlobKey key1;
    memset(key1.bytes, 0x55, sizeof(key1.bytes));
    C4SliceResult str = c4blob_keyToString(key1);
    CHECK(string((char*)str.buf, str.size) == "sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=");

    C4BlobKey key2;
    CHECK(c4blob_keyFromString(C4Slice{str.buf, str.size}, &key2));
    CHECK(memcmp(key1.bytes, key2.bytes, sizeof(key1.bytes)) == 0);
}


TEST_CASE("parse invalid blob keys", "[blob][C][!throws]") {
    ExpectingExceptions x;
    C4BlobKey key2;
    CHECK_FALSE(c4blob_keyFromString(C4STR(""), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("rot13-xxxx"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-VVVVVVVVVVVVVVVVVVVVVV"), &key2));
    CHECK_FALSE(c4blob_keyFromString(C4STR("sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVU"), &key2));
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "missing blobs", "[blob][C]") {
    ExpectingExceptions x;
    
    CHECK(c4blob_getSize(store, bogusKey) == -1);

    C4Error error;
    C4SliceResult data = c4blob_getContents(store, bogusKey, &error);
    CHECK(data.buf == nullptr);
    CHECK(data.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
    data = c4blob_getFilePath(store, bogusKey, &error);
    CHECK(data.buf == nullptr);
    CHECK(data.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "create blobs", "[blob][Encryption][C]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");

    // Add blob to the store:
    C4BlobKey key;
    C4Error error;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key, &error));

    auto str = c4blob_keyToString(key);
    CHECK(string((char*)str.buf, str.size) == "sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E=");
    CHECK(memcmp(c4blob_computeKey(blobToStore).bytes, key.bytes, 20) == 0);

    // Read it back and compare
    int64_t blobSize = c4blob_getSize(store, key);
    CHECK(blobSize >= blobToStore.size);
    if (encrypted)
        CHECK(blobSize <= blobToStore.size + 16);   // getSize is approximate in an encrypted store
    else
        CHECK(blobSize == blobToStore.size);
    
    auto gotBlob = c4blob_getContents(store, key, &error);
    REQUIRE(gotBlob.buf != nullptr);
    REQUIRE(gotBlob.size == blobToStore.size);
    CHECK(memcmp(gotBlob.buf, blobToStore.buf, gotBlob.size) == 0);

    C4SliceResult p = c4blob_getFilePath(store, key, &error);
    if (encrypted) {
        CHECK(p.buf == nullptr);
        CHECK(p.size == 0);
        CHECK(error.code == kC4ErrorWrongFormat);
    } else {
        REQUIRE(p.buf != nullptr);
        string path((char*)p.buf, p.size);
        string filename = "QneWo5IYIQ0ZrbCG0hXPGC6jy7E=.blob";
        CHECK(path.find(filename) == path.size() - filename.size());
    }

    // Try storing it again
    C4BlobKey key2;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key2, &error));
    CHECK(memcmp(&key2, &key, sizeof(key2)) == 0);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "delete blobs", "[blob][Encryption][C]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");
    
    // Add blob to the store:
    C4BlobKey key;
    C4Error error;
    REQUIRE(c4blob_create(store, blobToStore, nullptr, &key, &error));
    
    auto str = c4blob_keyToString(key);
    CHECK(string((char*)str.buf, str.size) == "sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E=");
    
    // Delete it
    REQUIRE(c4blob_delete(store, key, &error));
    
    // Try to read it (should be gone):
    int64_t blobSize = c4blob_getSize(store, key);
    CHECK(blobSize == -1);

    {
        ExpectingExceptions x;
        auto gotBlob = c4blob_getContents(store, key, &error);
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
    CHECK(c4blob_create(store, blobToStore, &expectedKey, &key, &error));
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "read blob with stream", "[blob][Encryption][C]") {
    string blob = "This is a blob to store in the store!";

    // Add blob to the store:
    C4BlobKey key;
    C4Error error;
    REQUIRE(c4blob_create(store, {blob.data(), blob.size()}, nullptr,  &key, &error));

    {
        ExpectingExceptions x;
        CHECK( c4blob_openReadStream(store, bogusKey, &error) == nullptr);
    }

    char buf[10000];
    size_t kReadSizes[5] = {1, 6, blob.size(), 4096, 10000};
    for (int i = 0; i < 5; i++) {
        size_t readSize = kReadSizes[i];

        auto stream = c4blob_openReadStream(store, key, &error);
        REQUIRE(stream);

        // Read it back, 6 bytes at a time:
        string readBack;
        size_t bytesRead;
        do {
            bytesRead = c4stream_read(stream, buf, readSize, &error);
            readBack.append(buf, bytesRead);
        } while (bytesRead == readSize);
        REQUIRE(error.code == 0);
        CHECK(readBack == blob);

        // Try seeking:
        REQUIRE(c4stream_seek(stream, 10, &error));
        REQUIRE(c4stream_read(stream, buf, 4, &error) == 4);
        CHECK(memcmp(buf, "blob", 4) == 0);

        CHECK(c4stream_getLength(stream, &error) == blob.size());

        c4stream_close(stream);
        c4stream_close(nullptr); // this should be a no-op, not a crash
    }
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blob with stream", "[blob][Encryption][C]") {
    // Write the blob:
    C4Error error;
    C4WriteStream *stream = c4blob_openWriteStream(store, &error);
    REQUIRE(stream);
    CHECK(c4stream_bytesWritten(stream) == 0);

    for (int i = 0; i < 1000; i++) {
        char buf[100];
        sprintf(buf, "This is line %03d.\n", i);
        REQUIRE(c4stream_write(stream, buf, strlen(buf), &error));
    }

    CHECK(c4stream_bytesWritten(stream) == 18*1000);

    // Get the blob key, and install it:
    C4BlobKey key = c4stream_computeBlobKey(stream);
    CHECK(c4stream_install(stream, nullptr, &error));
    c4stream_closeWriter(stream);
    c4stream_closeWriter(nullptr);

    C4SliceResult keyStr = c4blob_keyToString(key);
    CHECK(string((char*)keyStr.buf, keyStr.size) == "sha1-0htkjBHcrTyIk9K8e1zZq47yWxw=");
    c4slice_free(keyStr);

    // Read it back using the key:
    C4SliceResult contents = c4blob_getContents(store, key, &error);
    CHECK(contents.size == 18*1000);
    c4slice_free(contents);

    // Read it back random-access:
    C4ReadStream *reader = c4blob_openReadStream(store, key, &error);
    REQUIRE(reader);
    static const int increment = 3*3*3*3;
    int line = increment;
    for (uint64_t i = 0; i < 1000; i++) {
        line = (line + increment) % 1000;
        INFO("Reading line " << line << " at offset " << 18*line);
        char buf[100], readBuf[100];
        sprintf(buf, "This is line %03d.\n", line);
        REQUIRE(c4stream_seek(reader, 18*line, &error));
        REQUIRE(c4stream_read(reader, readBuf, 18, &error) == 18);
        readBuf[18] = '\0';
        REQUIRE(string(readBuf) == string(buf));
    }
    c4stream_close(reader);
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blobs of many sizes", "[blob][Encryption][C]") {
    // The interesting sizes for encrypted blobs are right around the file block size (4096)
    // and the cipher block size (16).
    const vector<size_t> kSizes = {0, 1, 15, 16, 17, 4095, 4096, 4097,
                                   4096+15, 4096+16, 4096+17, 8191, 8192, 8193};
    for (size_t size : kSizes) {
        //Log("---- %lu-byte blob", size);
        INFO("Testing " << size << "-byte blob");
        // Write the blob:
        C4Error error;
        C4WriteStream *stream = c4blob_openWriteStream(store, &error);
        REQUIRE(stream);

        const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXY";
        for (int i = 0; i < size; i++) {
            int c = i % strlen(chars);
            REQUIRE(c4stream_write(stream, &chars[c], 1, &error));
        }

        // Get the blob key, and install it:
        C4BlobKey key = c4stream_computeBlobKey(stream);
        CHECK(c4stream_install(stream, nullptr, &error));
        c4stream_closeWriter(stream);

        // Read it back using the key:
        C4SliceResult contents = c4blob_getContents(store, key, &error);
        CHECK(contents.size == size);
        for (int i = 0; i < size; i++)
            CHECK(((const char*)contents.buf)[i] == chars[i % strlen(chars)]);
        c4slice_free(contents);
    }
}


N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write blob and cancel", "[blob][Encryption][C]") {
    // Write the blob:
    C4Error error;
    C4WriteStream *stream = c4blob_openWriteStream(store, &error);
    REQUIRE(stream);

    const char *buf = "This is line oops\n";
    CHECK(c4stream_write(stream, buf, strlen(buf), &error));

    c4stream_closeWriter(stream);
}

N_WAY_TEST_CASE_METHOD(BlobStoreTest, "write identical blob", "[blob][C]") {
    // CBL-670: Installing identical blob can cause filesystem issues on Windows,
    // but this is hard to reproduce, so simulate having the file open (which is what
    // somehow ends up happening)

    if(encrypted) {
        // Can't get file paths with encryption, and can't compile BlobStoreTest with
        // TEST_CASE_METHOD macro
        return;
    }

    C4Error error;
    const int streamCount = 2;
    C4WriteStream *streams[streamCount];
    for(int iter = 0; iter < streamCount; iter++) {
        C4WriteStream *stream = c4blob_openWriteStream(store, &error);
        REQUIRE(stream);
        CHECK(c4stream_bytesWritten(stream) == 0);
        streams[iter] = stream;

        for (int i = 0; i < 1000; i++) {
            char buf[100];
            sprintf(buf, "This is line %03d.\n", i);
            REQUIRE(c4stream_write(stream, buf, strlen(buf), &error));
        }

        CHECK(c4stream_bytesWritten(stream) == 18*1000);
        C4BlobKey key = c4stream_computeBlobKey(stream);
        if(iter > 0) {
            auto path = c4blob_getFilePath(store, key, &error);
            REQUIRE(path.buf != nullptr);
            auto pathStr = string((char *)path.buf, path.size);

            // Simulate the file being in use
            ifstream fin(pathStr);
            CHECK(c4stream_install(stream, nullptr, &error));
        } else {
            CHECK(c4stream_install(stream, nullptr, &error));
        }

        C4SliceResult keyStr = c4blob_keyToString(key);
        CHECK(string((char*)keyStr.buf, keyStr.size) == "sha1-0htkjBHcrTyIk9K8e1zZq47yWxw=");
        c4slice_free(keyStr);

        // Read it back using the key:
        C4SliceResult contents = c4blob_getContents(store, key, &error);
        CHECK(contents.size == 18*1000);
        c4slice_free(contents);
    }

    for (auto& stream : streams) {
        c4stream_closeWriter(stream);
    }
}