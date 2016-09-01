//
//  c4BlobStoreTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4BlobStore.h"

using namespace std;


class BlobStoreTest {
public:
    
    BlobStoreTest() {
        C4Error error;
        store = c4blob_openStore(c4str(kTestDir "cbl_blob_test/"), kC4DB_Create, nullptr, &error);
        CHECK(store != nullptr);
    }

    ~BlobStoreTest() {
        C4Error error;
        CHECK(c4blob_deleteStore(store, &error));
    }

    C4BlobStore *store {nullptr};
};


TEST_CASE("parse blob keys", "[blob]") {
    C4BlobKey key1;
    memset(key1.bytes, 0x55, sizeof(key1.bytes));
    C4SliceResult str = c4blob_keyToString(key1);
    CHECK(string((char*)str.buf, str.size) == "sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=");

    C4BlobKey key2;
    CHECK(c4blob_keyFromString(C4Slice{str.buf, str.size}, &key2));
    CHECK(memcmp(key1.bytes, key2.bytes, sizeof(key1.bytes)) == 0);
}


TEST_CASE_METHOD(BlobStoreTest, "missing blobs", "[blob]") {
    C4BlobKey bogusKey;
    memset(bogusKey.bytes, 0x55, sizeof(bogusKey.bytes));

    CHECK(c4blob_getSize(store, bogusKey) == -1);

    C4Error error;
    C4SliceResult data = c4blob_getContents(store, bogusKey, &error);
    CHECK(data.buf == nullptr);
    CHECK(data.size == 0);
    CHECK(error.code == kC4ErrorNotFound);
}


TEST_CASE_METHOD(BlobStoreTest, "create blobs", "[blob]") {
    C4Slice blobToStore = C4STR("This is a blob to store in the store!");

    // Add blob to the store:
    C4BlobKey key;
    C4Error error;
    CHECK(c4blob_create(store, blobToStore, &key, &error));

    auto str = c4blob_keyToString(key);
    CHECK(string((char*)str.buf, str.size) == "sha1-QneWo5IYIQ0ZrbCG0hXPGC6jy7E=");

    // Read it back and compare
    CHECK(c4blob_getSize(store, key) == blobToStore.size);
    
    auto gotBlob = c4blob_getContents(store, key, &error);
    CHECK(gotBlob.buf != nullptr);
    CHECK(gotBlob.size == blobToStore.size);
    CHECK(memcmp(gotBlob.buf, blobToStore.buf, gotBlob.size) == 0);

    // Try storing it again
    C4BlobKey key2;
    CHECK(c4blob_create(store, blobToStore, &key2, &error));
    CHECK(memcmp(&key2, &key, sizeof(key2)) == 0);
}
