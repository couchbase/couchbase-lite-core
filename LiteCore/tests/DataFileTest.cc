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
#include "Fleece.hh"
#include "Benchmark.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DbInfo", "[DataFile]") {
    REQUIRE(db->isOpen());
    REQUIRE(&store->dataFile() == db);
    REQUIRE(store->recordCount() == 0);
    REQUIRE(store->lastSequence() == 0);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Delete DB", "[DataFile]") {
    auto path = db->filePath();
    db->deleteDataFile();
    delete db;
    db = nullptr;
    path.forEachMatch([](const FilePath &file) {
        FAIL("Leftover file(s) '" << file.path() << "' after deleting database");
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile CreateDoc", "[DataFile]") {
    alloc_slice key("key");
    {
        Transaction t(db);
        store->set(key, "value"_sl, t);
        t.commit();
    }
    REQUIRE(store->lastSequence() == 1);
    Record rec = db->defaultKeyStore().get(key);
    REQUIRE(rec.key() == key);
    REQUIRE(rec.body() == "value"_sl);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile SaveDocs", "[DataFile]") {
    {
        //WORKAROUND: Add a rec before the main transaction so it doesn't start at sequence 0
        Transaction t(db);
        store->set("a"_sl, "A"_sl, t);
        t.commit();
    }

    unique_ptr<DataFile> aliased_db { newDatabase(db->filePath()) };
    REQUIRE(aliased_db->defaultKeyStore().get("a"_sl).body() == alloc_slice("A"));

    {
        Transaction t(db);
        Record rec("rec"_sl);
        rec.setVersion("m-e-t-a"_sl);
        rec.setBody("THIS IS THE BODY"_sl);
        store->write(rec, t);

        REQUIRE(rec.sequence() == 2);
        REQUIRE(store->lastSequence() == 2);
        auto doc_alias = store->get(rec.sequence());
        REQUIRE(doc_alias.key() == rec.key());
        REQUIRE(doc_alias.version() == rec.version());
        REQUIRE(doc_alias.body() == rec.body());

        doc_alias.setBody("NU BODY"_sl);
        store->write(doc_alias, t);

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

static void createNumberedDocs(KeyStore *store) {
    Transaction t(store->dataFile());
    for (int i = 1; i <= 100; i++) {
        string docID = stringWithFormat("rec-%03d", i);
        sequence_t seq = store->set(slice(docID), slice(docID), t);
        REQUIRE(seq == (sequence_t)i);
        REQUIRE(store->get(slice(docID)).body() == alloc_slice(docID));
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
        opts.contentOptions = metaOnly ? kMetaOnly : kDefaultContent;

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
    opts.descending = true;

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
    {
        Transaction t(db);
        store->set("a"_sl, "A"_sl, t);
        t.commit();
    }
    {
        Transaction t(db);
        store->set("x"_sl, "X"_sl, t);
        store->set("a"_sl, "Z"_sl, t);
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
        Transaction trans(db);
        for (unsigned d = 1; d <= kNDocs; d++) {
            string docID = stringWithFormat("%03lu.%03lu", (unsigned long)t, (unsigned long)d);
            store->set(slice(docID), "some record content goes here"_sl, trans);
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
        Transaction t(db);
        store->set(key, "A"_sl, t);
        t.commit();
    }
    REQUIRE(store->lastSequence() == 1);
    {
        Transaction t(db);
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
        Transaction t(db);
        store->set(key, "A"_sl, t);
        t.commit();
    }

    {
        Transaction t(db);
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
        Transaction t(db);
        store->set(key, "A"_sl, t);
        t.commit();
    }

    {
        Transaction t(db);
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


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreWrite", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    {
        Transaction t(db);
        s.set(key, "value"_sl, t);
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
        Transaction t(db);
        newSeq = s.set(key, "initialvalue"_sl, t, &oldSeq);
        CHECK(newSeq == 1);

        auto badSeq = s.set(key, "wronginitialvalue"_sl, t, &oldSeq);
        CHECK(badSeq == 0);
        t.commit();
    }

    REQUIRE(s.lastSequence() == 1);
    Record rec = s.get(key);
    REQUIRE(rec.body() == "initialvalue"_sl);

    {
        Transaction t(db);
        newSeq = s.set(key, "updatedvalue"_sl, t, &newSeq);
        CHECK(newSeq == 2);
        t.commit();
    }

    REQUIRE(s.lastSequence() == 2);
    Record rec2 = s.get(key);
    REQUIRE(rec2.body() == "updatedvalue"_sl);
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
        Transaction t(db);
        store->set("key"_sl, "value"_sl, t);
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
        Transaction t(db);
        store->set("key"_sl, "somethingelse"_sl, t);
        t.commit();
    });

    // Now try to open a nonexistent db, read-only:
    ExpectException(error::LiteCore, error::CantOpenFile, [&]{
        (void)newDatabase("/tmp/db_non_existent", &options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Compact", "[DataFile]") {
    createNumberedDocs(store);

    {
        Transaction t(db);
        for (int i = 1; i <= 100; i += 3) {
            auto docID = stringWithFormat("rec-%03d", i);
            Record rec = store->get((slice)docID);
            store->del(rec, t);
        }
        t.commit();
    }

    db->compact();
}


#pragma mark - ENCRYPTION:


#ifdef COUCHBASE_ENTERPRISE


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Unsupported Encryption", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kNoEncryption));
    REQUIRE(!factory().encryptionEnabled(kAES256));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = "12345678901234567890123456789012"_sl;
    ExpectException(error::LiteCore, error::UnsupportedEncryption, [&]{
        reopenDatabase(&options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Open Unencrypted With Key", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kNoEncryption));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES128;
    options.encryptionKey = "1234567890123456"_sl;
    ExpectException(error::LiteCore, error::NotADatabaseFile, [&]{
        reopenDatabase(&options);
    });
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Encryption", "[DataFile][Encryption][!throws]") {
    REQUIRE(factory().encryptionEnabled(kAES128));
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES128;
    options.encryptionKey = "1234567890123456"_sl;
    auto dbPath = databasePath("encrypted");
    deleteDatabase(dbPath);
    try {
        {
            // Create encrypted db:
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
            Transaction t(*encryptedDB);
            encryptedDB->defaultKeyStore().set("k"_sl, "value"_sl, t);
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
    REQUIRE(factory().encryptionEnabled(kAES128));
    auto dbPath = db->filePath();
    auto options = db->options();
    createNumberedDocs(store);

    options.encryptionAlgorithm = kAES128;
    options.encryptionKey = alloc_slice(kEncryptionKeySize[kAES128]);
    randomBytes(options.encryptionKey);

    db->rekey(options.encryptionAlgorithm, options.encryptionKey);

    reopenDatabase(&options);

    Record rec = store->get((slice)"rec-001");
    REQUIRE(rec.exists());
    
    // Change encryption key
    randomBytes(options.encryptionKey);
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
    REQUIRE(!factory().encryptionEnabled(kAES128));
    REQUIRE(!factory().encryptionEnabled(kAES256));

    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES128;
    options.encryptionKey = "1234567890123456"_sl;
    ExpectException(error::LiteCore, error::UnsupportedEncryption, [&]{
        reopenDatabase(&options);
    });
}


#endif // COUCHBASE_ENTERPRISE

