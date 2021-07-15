//
// DataFile_Test.cc
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#include "DataFile.hh"
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "FleeceImpl.hh"
#include "Benchmark.hh"
#include "SecureRandomize.hh"
#ifndef _MSC_VER
#include <sys/stat.h>
#endif

#include "LiteCoreTest.hh"
#include <sstream>
#include <cinttypes>

using namespace litecore;
using namespace fleece::impl;
using namespace std;

static void check_parent(string full, string parent)
{
#ifdef _MSC_VER
    replace(full.begin(), full.end(), '/', '\\');
    replace(parent.begin(), parent.end(), '/', '\\');
#endif

    CHECK(FilePath(full).parentDir().path() == parent);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DbInfo", "[DataFile]") {
    REQUIRE(db->isOpen());
    REQUIRE(&store->dataFile() == db.get());
    REQUIRE(store->recordCount() == 0);
    REQUIRE(store->lastSequence() == 0);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Delete DB", "[DataFile]") {
    auto path = db->filePath();
    deleteDatabase();
    path.forEachMatch([](const FilePath &file) {
        FAIL("Leftover file(s) '" << file.path() << "' after deleting database");
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile CreateDoc", "[DataFile]") {
    alloc_slice key("key");
    {
        ExclusiveTransaction t(db);
        RecordUpdate rec(key, "body"_sl);
        rec.version = "version"_sl;
        rec.extra = "extra"_sl;
        CHECK(store->set(rec, true, t) == 1);
        t.commit();
    }
    CHECK(store->lastSequence() == 1);

    // Get by key:
    Record rec = db->defaultKeyStore().get(key, kMetaOnly);
    REQUIRE(rec.exists());
    CHECK(rec.key() == key);
    CHECK(rec.body() == nullslice);
    CHECK(rec.bodySize() == 4);
    CHECK(rec.version() == "version"_sl);
    CHECK(rec.extra() == nullslice);
    CHECK(rec.sequence() == 1);

    rec = db->defaultKeyStore().get(key, kCurrentRevOnly);
    REQUIRE(rec.exists());
    CHECK(rec.key() == key);
    CHECK(rec.body() == "body"_sl);
    CHECK(rec.bodySize() == 4);
    CHECK(rec.version() == "version"_sl);
    CHECK(rec.extra() == nullslice);
    CHECK(rec.sequence() == 1);

    rec = db->defaultKeyStore().get(key, kEntireBody);
    REQUIRE(rec.exists());
    CHECK(rec.key() == key);
    CHECK(rec.body() == "body"_sl);
    CHECK(rec.bodySize() == 4);
    CHECK(rec.version() == "version"_sl);
    CHECK(rec.extra() == "extra"_sl);
    CHECK(rec.sequence() == 1);

    // Get by sequence:
    rec = db->defaultKeyStore().get(1);
    REQUIRE(rec.exists());
    CHECK(rec.key() == key);
    CHECK(rec.body() == "body"_sl);
    CHECK(rec.bodySize() == 4);
    CHECK(rec.version() == "version"_sl);
    CHECK(rec.extra() == "extra"_sl);
    CHECK(rec.sequence() == 1);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile SaveDocs", "[DataFile]") {
    {
        //WORKAROUND: Add a rec before the main transaction so it doesn't start at sequence 0
        ExclusiveTransaction t(db);
        Record rec("a"_sl);
        rec.setBody("A"_sl);
        store->set(rec, true, t);
        t.commit();
    }

    unique_ptr<DataFile> aliased_db { newDatabase(db->filePath()) };
    REQUIRE(aliased_db->defaultKeyStore().get("a"_sl).body() == alloc_slice("A"));

    {
        ExclusiveTransaction t(db);
        Record rec("rec"_sl);
        rec.setVersion("m-e-t-a"_sl);
        rec.setBody("THIS IS THE BODY"_sl);
        store->set(rec, true, t);

        REQUIRE(rec.sequence() == 2);
        REQUIRE(store->lastSequence() == 2);
        auto doc_alias = store->get(rec.sequence());
        REQUIRE(doc_alias.key() == rec.key());
        REQUIRE(doc_alias.version() == rec.version());
        REQUIRE(doc_alias.body() == rec.body());

        doc_alias.setBody("NU BODY"_sl);
        store->set(doc_alias, true, t);

        REQUIRE(store->read(rec));
        REQUIRE(rec.sequence() == 3);
        REQUIRE(rec.version() == doc_alias.version());
        REQUIRE(rec.body() == doc_alias.body());

        // Record shouldn't exist outside transaction yet:
        REQUIRE(aliased_db->defaultKeyStore().get("rec"_sl).sequence() == 0);
        t.commit();
    }

    REQUIRE(store->get("rec"_sl).sequence() == 3);
    REQUIRE(aliased_db->defaultKeyStore().get("rec"_sl).sequence() == 3);
}


static void createNumberedDocs(KeyStore *store, int n =100, bool withAssertions =true) {
    ExclusiveTransaction t(store->dataFile());
    for (int i = 1; i <= n; i++) {
        string docID = stringWithFormat("rec-%03d", i);
        RecordUpdate rec(docID, docID);
        sequence_t seq = store->set(rec, true, t);
        if (withAssertions) {
            REQUIRE(seq == (sequence_t)i);
            REQUIRE(store->get(slice(docID)).body() == slice(docID));
        }
    }
    t.commit();
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile EnumerateDocs", "[DataFile]") {
    {
        INFO("Enumerate empty db");
        int i = 0;
        RecordEnumerator e(*store);
        for (; e.next(); ++i) {
            FAIL("Shouldn't have found any docs");
        }
        REQUIRE_FALSE(e);
    }

    createNumberedDocs(store);

    for (int metaOnly=0; metaOnly <= 1; ++metaOnly) {
        INFO("Enumerate over all docs, metaOnly=" << metaOnly);
        RecordEnumerator::Options opts;
        opts.contentOption = metaOnly ? kMetaOnly : kEntireBody;

        {
            int i = 1;
            RecordEnumerator e(*store, opts);
            for (; e.next(); ++i) {
                string expectedDocID = stringWithFormat("rec-%03d", i);
                REQUIRE(e->key() == alloc_slice(expectedDocID));
                REQUIRE(e->sequence() == (sequence_t)i);
                REQUIRE(e->bodySize() > 0); // even metaOnly should set the body size
            }
            REQUIRE(i == 101);
            REQUIRE_FALSE(e);
        }
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile EnumerateDocsDescending", "[DataFile]") {
    RecordEnumerator::Options opts;
    opts.sortOption = kDescending;

    createNumberedDocs(store);

    SECTION("Enumerate over all docs, descending:") {
        int i = 100;
        for (RecordEnumerator e(*store, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence_t)i);
        }
        REQUIRE(i == 0);
    }

}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile AbortTransaction", "[DataFile]") {
    // Initial record:
    Record a("a");
    a.setBody("A");
    {
        ExclusiveTransaction t(db);
        store->set(a, true, t);
        t.commit();
    }
    {
        ExclusiveTransaction t(db);
        createDoc("x"_sl, "X"_sl, t);
        a.setBody("Z");
        store->set(a, true, t);
        REQUIRE(store->get("a"_sl).body() == alloc_slice("Z"));
        REQUIRE(store->get("a"_sl).body() == alloc_slice("Z"));
        t.abort();
    }
    REQUIRE(store->get("a"_sl).body() == alloc_slice("A"));
    REQUIRE(store->get("x"_sl).sequence() == 0);
}


// Test for MB-12287
N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile TransactionsThenIterate", "[DataFile]") {
    unique_ptr<DataFile> db2 { newDatabase(db->filePath()) };

    const unsigned kNTransactions = 42; // 41 is ok, 42+ fails
    const unsigned kNDocs = 100;

    for (unsigned t = 1; t <= kNTransactions; t++) {
        ExclusiveTransaction trans(db);
        for (unsigned d = 1; d <= kNDocs; d++) {
            string docID = stringWithFormat("%03lu.%03lu", (unsigned long)t, (unsigned long)d);
            createDoc(slice(docID), "some record content goes here"_sl, trans);
        }
        trans.commit();
    }

    int i = 0;
    for (RecordEnumerator iter(db2->defaultKeyStore()); iter.next(); ) {
        slice key = (*iter).key();
        //INFO("key = %s", key);
        unsigned t = (i / kNDocs) + 1;
        unsigned d = (i % kNDocs) + 1;
        REQUIRE(key == slice(stringWithFormat("%03lu.%03lu",
                                              (unsigned long)t, (unsigned long)d)));
        i++;
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile DeleteKey", "[DataFile]") {
    slice key("a");
    {
        ExclusiveTransaction t(db);
        createDoc(key, "A"_sl, t);
        t.commit();
    }
    REQUIRE(store->lastSequence() == 1);
    {
        ExclusiveTransaction t(db);
        store->del(key, t);
        t.commit();
    }
    Record rec = store->get(key);
    REQUIRE_FALSE(rec.exists());
    REQUIRE(store->lastSequence() == 1);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile DeleteDoc", "[DataFile]") {
    slice key("a");
    {
        ExclusiveTransaction t(db);
        createDoc(key, "A"_sl, t);
        t.commit();
    }

    {
        ExclusiveTransaction t(db);
        Record rec = store->get(key);
        store->del(rec, t);
        t.commit();
    }

    Record rec = store->get(key);
    //    REQUIRE(rec.deleted());
    REQUIRE_FALSE(rec.exists());
}


// Tests workaround for ForestDB bug MB-18753
N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile DeleteDocAndReopen", "[DataFile]") {
    slice key("a");
    {
        ExclusiveTransaction t(db);
        createDoc(key, "A"_sl, t);
        t.commit();
    }

    {
        ExclusiveTransaction t(db);
        Record rec = store->get(key);
        store->del(rec, t);
        t.commit();
    }

    Record rec = store->get(key);
    //    REQUIRE(rec.deleted());
    REQUIRE_FALSE(rec.exists());

    reopenDatabase();

    Record doc2 = store->get(key);
    //    REQUIRE(doc2.deleted());
    REQUIRE_FALSE(doc2.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreInfo", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    REQUIRE(s.lastSequence() == 0);
    REQUIRE(s.name() == string("store"));

    REQUIRE(s.recordCount() == 0);
    REQUIRE(s.lastSequence() == 0);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStore Create-Then-Abort", "[DataFile]") {
    {
        ExclusiveTransaction t(db);
        KeyStore &s = db->getKeyStore("store", KeyStore::noSequences);
        s.setKV("key"_sl, "value"_sl, t);
        t.abort();
    }
    // KeyStore's table `kv_store` doesn't exist on disk because the transaction was aborted.
    // Now try to write to it again -- the KeyStore should repeat the CREATE TABLE command.
    {
        ExclusiveTransaction t(db);
        KeyStore &s = db->getKeyStore("store", KeyStore::noSequences);
        s.setKV("key"_sl, "value"_sl, t);
        t.commit();
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreWrite", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    {
        ExclusiveTransaction t(db);
        createDoc(s, key, "value"_sl, t);
        t.commit();
    }
    REQUIRE(s.lastSequence() == 1);
    Record rec = s.get(key);
    REQUIRE(rec.key() == key);
    REQUIRE(rec.body() == "value"_sl);

    Record doc2 = store->get(key);
    REQUIRE_FALSE(doc2.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Conditional Write", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    sequence_t oldSeq = 0;
    sequence_t newSeq;
    {
        ExclusiveTransaction t(db);
        RecordUpdate rec(key, "initialvalue"_sl);
        rec.sequence = oldSeq;
        newSeq = s.set(rec, true, t);
        CHECK(newSeq == 1);

        rec.body = "wronginitialvalue"_sl;
        rec.sequence = oldSeq;
        auto badSeq = s.set(rec, true, t);
        CHECK(badSeq == 0);
        t.commit();
    }

    REQUIRE(s.lastSequence() == 1);
    REQUIRE(s.get(key).body() == "initialvalue"_sl);

    {
        ExclusiveTransaction t(db);
        RecordUpdate rec(key, "updatedvalue"_sl);
        rec.sequence = newSeq;
        newSeq = s.set(rec, true, t);
        CHECK(newSeq == 2);
        t.commit();
    }

    REQUIRE(s.lastSequence() == 2);
    REQUIRE(s.get(key).body() == "updatedvalue"_sl);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Move Record", "[DataFile]") {
    {
        ExclusiveTransaction t(db);
        createDoc("xxx", "x", t);

        RecordUpdate rec("key", "value", DocumentFlags::kHasAttachments);
        rec.version = "version";
        rec.extra = "extra";
        sequence_t seq = store->set(rec, true, t);
        CHECK(seq == 2);
        t.commit();
    }
    CHECK(store->lastSequence() == 2);
    CHECK(store->purgeCount() == 0);

    KeyStore& otherStore = db->getKeyStore("other");
    CHECK(otherStore.lastSequence() == 0);
    CHECK(otherStore.purgeCount() == 0);

    {
        ExclusiveTransaction t(db);
        store->moveTo("key", otherStore, t, "newKey");
        t.commit();
    }

    Record rec = otherStore.get("newKey");
    REQUIRE(rec.exists());
    CHECK(rec.key() == "newKey");
    CHECK(rec.body() == "value");
    CHECK(rec.version() == "version");
    CHECK(rec.extra() == "extra");
    CHECK(rec.flags() == DocumentFlags::kHasAttachments);
    CHECK(rec.sequence() == 1);
    CHECK(otherStore.lastSequence() == 1);

    rec = store->get("key");
    CHECK_FALSE(rec.exists());
    CHECK(store->lastSequence() == 2);
    CHECK(store->purgeCount() == 1);

    {
        ExclusiveTransaction t(db);

        // Moving a nonexistent record:
        try {
            ExpectingExceptions x;
            store->moveTo("key", otherStore, t, "bogus");
            FAIL_CHECK("Moving nonexistent record didn't throw");
        } catch (const error &x) {
            CHECK(x.domain == error::LiteCore);
            CHECK(x.code == error::NotFound);
        }

        // Moving onto an existing record:
        try {
            ExpectingExceptions x;
            store->moveTo("xxx", otherStore, t, "newKey");
        } catch (const error &x) {
            CHECK(x.domain == error::LiteCore);
            CHECK(x.code == error::Conflict);
        }
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreDelete", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
//    {
//        Transaction t(db);
//        t(s).set(key, "value"_sl);
//        t.commit();
//    }
    s.erase();
    REQUIRE(s.lastSequence() == 0);
    Record rec = s.get(key);
    REQUIRE_FALSE(rec.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreAfterClose", "[DataFile][!throws]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    db->close();
    ExpectException(error::LiteCore, error::NotOpen, [&]{
        Record rec = s.get(key);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile ReadOnly", "[DataFile][!throws]") {
    {
        ExclusiveTransaction t(db);
        createDoc("key"_sl, "value"_sl, t);
        t.commit();
    }
    // Reopen db as read-only:
    auto options = db->options();
    options.writeable = false;
    options.create = false;
    reopenDatabase(&options);

    auto rec = store->get("key"_sl);
    REQUIRE(rec.exists());

    // Attempt to change a rec:
    ExpectException(error::LiteCore, error::NotWriteable, [&]{
        ExclusiveTransaction t(db);
        createDoc("key"_sl, "somethingelse"_sl, t);
        t.commit();
    });

    // Now try to open a nonexistent db, read-only:
    ExpectException(error::LiteCore, error::CantOpenFile, [&]{
        (void)newDatabase(FilePath("/tmp/db_non_existent"), &options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Compact", "[DataFile]") {
    createNumberedDocs(store, 10000, false);

    {
        ExclusiveTransaction t(db);
        for (int i = 2000; i <= 7000; i++) {
            auto docID = stringWithFormat("rec-%03d", i);
            Record rec = store->get((slice)docID);
            store->del(rec, t);
        }
        t.commit();
    }

    int64_t oldSize = db->fileSize();

    SECTION("Close & reopen (incremental vacuum on close)") {
        reopenDatabase();
    }
    SECTION("Compact database (vacuum)") {
        db->maintenance(DataFile::kCompact);
    }

    int64_t newSize = db->fileSize();
    Log("File size went from %" PRIi64 " to %" PRIi64, oldSize, newSize);
    CHECK(newSize < oldSize - 100000);
}

TEST_CASE("CanonicalPath") {
#ifdef _MSC_VER
    const char* startPath = "C:\\folder\\..\\subfolder\\";
    string endPath = "C:\\subfolder\\";
#else
    auto tmpPath = TestFixture::sTempDir.path();
    auto startPath = tmpPath + "folder/";
    ::mkdir(startPath.c_str(), 777);
    startPath += "../subfolder/";
    auto endPath = tmpPath + "subfolder";
    ::mkdir(endPath.c_str(), 777);
#if __APPLE__ && !TARGET_OS_IPHONE
    endPath = "/private" + endPath;
#endif
#endif

    FilePath path(startPath);
    CHECK(path.canonicalPath() == endPath);

#ifdef _MSC_VER
    startPath = u8"C:\\日本語\\";
    endPath = startPath;
#else
    startPath = tmpPath + u8"日本語";
    ::mkdir(startPath.c_str(), 777);
    endPath = startPath;
#if __APPLE__ && !TARGET_OS_IPHONE
    endPath = "/private" + endPath;
#endif
#endif

    path = FilePath(startPath);
    CHECK(path.canonicalPath() == endPath);
}

TEST_CASE("ParentDir") {
#ifdef _MSC_VER
    CHECK(FilePath("C:\\").parentDir().path() == "C:\\");
    CHECK(FilePath("D:\\folder\\subfolder\\file").parentDir().path() == "D:\\folder\\subfolder\\");
    CHECK(FilePath("C:\\folder\\subfolder\\").parentDir().path() == "C:\\folder\\");
    CHECK(FilePath("C:\\folder\\file").parentDir().path() == "C:\\folder\\");
#else
    CHECK(FilePath("/").parentDir().path() == "/");
    CHECK(FilePath("/folder/subfolder/file").parentDir().path() == "/folder/subfolder/");
    CHECK(FilePath("/folder/subfolder/").parentDir().path() == "/folder/");
    CHECK(FilePath("/folder/file").parentDir().path() == "/folder/");
#endif

    check_parent("folder/subfolder/", "folder/");
    check_parent("folder/file", "folder/");
    check_parent("./file", "./");
    check_parent("./folder/", "./");
    check_parent("file", "./");
    check_parent("folder/", "./");
    ExpectException(error::POSIX, EINVAL, [&]{
        stringstream ss;
        ss << "." << FilePath::kSeparator;
        FilePath(ss.str()).parentDir();
    });
}

#pragma mark - ENCRYPTION:


#ifdef COUCHBASE_ENTERPRISE


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Unsupported Encryption", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kNoEncryption));
    REQUIRE(!factory().encryptionEnabled((EncryptionAlgorithm)2));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = (EncryptionAlgorithm)2;
    options.encryptionKey = "12345678901234567890123456789012"_sl;
    ExpectException(error::LiteCore, error::UnsupportedEncryption, [&]{
        reopenDatabase(&options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Open Unencrypted With Key", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kNoEncryption));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = "12345678901234567890123456789012"_sl;
    ExpectException(error::LiteCore, error::NotADatabaseFile, [&]{
        reopenDatabase(&options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Encryption", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kAES256));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = "12345678901234567890123456789012"_sl;
    auto dbPath = databasePath("encrypted");
    deleteDatabase(dbPath);
    try {
        {
            // Create encrypted db:
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
            ExclusiveTransaction t(*encryptedDB);
            createDoc(encryptedDB->defaultKeyStore(), "k"_sl, "value"_sl, t);
            t.commit();
        }
        {
            // Reopen with correct key:
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
            auto rec = encryptedDB->defaultKeyStore().get("k"_sl);
            REQUIRE(rec.body() == alloc_slice("value"));
        }
        {
            // Reopen without key:
            options.encryptionAlgorithm = kNoEncryption;
            ExpectException(error::LiteCore, error::NotADatabaseFile, [&]{
                unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
            });
        }
    } catch (...) {
        deleteDatabase(dbPath);
        throw;
    }
    deleteDatabase(dbPath);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Rekey", "[DataFile][Encryption]") {
    REQUIRE(factory().encryptionEnabled(kAES256));
    auto dbPath = db->filePath();
    auto options = db->options();
    createNumberedDocs(store);

    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = alloc_slice(kEncryptionKeySize[kAES256]);
    SecureRandomize(mutable_slice(options.encryptionKey));

    db->rekey(options.encryptionAlgorithm, options.encryptionKey);

    reopenDatabase(&options);

    Record rec = store->get((slice)"rec-001");
    REQUIRE(rec.exists());
    
    // Change encryption key
    SecureRandomize(mutable_slice(options.encryptionKey));
    db->rekey(options.encryptionAlgorithm, options.encryptionKey);
    
    reopenDatabase(&options);
    
    Record rec2 = store->get((slice)"rec-001");
    REQUIRE(rec2.exists());
    
    // Remove encryption
    options.encryptionAlgorithm = kNoEncryption;
    options.encryptionKey = nullslice;
    db->rekey(options.encryptionAlgorithm, options.encryptionKey);
    
    reopenDatabase(&options);
    
    Record rec3 = store->get((slice)"rec-001");
    REQUIRE(rec3.exists());
    
    // No-op, adding no encryption to a database with no encryption
    db->rekey(options.encryptionAlgorithm, options.encryptionKey);
    
    reopenDatabase(&options);
    
    Record rec4 = store->get((slice)"rec-001");
    REQUIRE(rec4.exists());
}


#else //!COUCHBASE_ENTERPRISE


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Verify Encryption Unsupported", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kNoEncryption));
    REQUIRE(!factory().encryptionEnabled(kAES256));

    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = "1234567890123456"_sl;
    ExpectException(error::LiteCore, error::UnsupportedEncryption, [&]{
        reopenDatabase(&options);
    });
}


#endif // COUCHBASE_ENTERPRISE


#pragma mark - MISC.


N_WAY_TEST_CASE_METHOD(DataFileTestFixture, "JSON null chars", "[Upgrade]") {
    // For https://github.com/couchbase/couchbase-lite-core/issues/528
    Encoder enc;
    JSONConverter converter(enc);
    bool ok = converter.encodeJSON("{\"foo\":\"Hello\\u0000There\"}"_sl);
    INFO("JSONConverter error " << converter.errorCode() << " '" << converter.errorMessage() << "' at pos " << converter.errorPos());
    REQUIRE(ok);
    auto data = enc.finish();
    REQUIRE(data);
    auto root = Value::fromData(data);
    REQUIRE(root);
    CHECK(root->asDict()->get("foo"_sl)->asString() == "Hello\0There"_sl);
}

N_WAY_TEST_CASE_METHOD(DataFileTestFixture, "Index table creation", "[Upgrade]") {
    // https://issues.couchbase.com/browse/CBL-550

    // Create an index, which triggers the logic to upgrade to version 301, which should not happen (and was)
    // if the version is already higher
    IndexSpec::Options options { "en", true };
    store->createIndex("num", alloc_slice("[[\".num\"]]"), IndexSpec::kValue, &options);

    // Before the fix, this would throw
    reopenDatabase();
}
