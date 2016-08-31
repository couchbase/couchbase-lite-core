//
//  DataFile_Test.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "DataFile.hh"
#include "DocEnumerator.hh"
#include "Error.hh"
#include "FilePath.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DbInfo", "[DataFile]") {
    REQUIRE(db->isOpen());
    REQUIRE_FALSE(db->isCompacting());
    REQUIRE_FALSE(DataFile::isAnyCompacting());
    REQUIRE(db->purgeCount() == 0);
    REQUIRE(&store->dataFile() == db);
    REQUIRE(store->documentCount() == 0);
    REQUIRE(store->lastSequence() == 0);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile CreateDoc", "[DataFile]") {
    alloc_slice key("key");
    {
        Transaction t(db);
        store->set(key, slice("value"), t);
    }
    REQUIRE(store->lastSequence() == 1);
    Document doc = db->defaultKeyStore().get(key);
    REQUIRE(doc.key() == key);
    REQUIRE(doc.body() == slice("value"));
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile SaveDocs", "[DataFile]") {
    {
        //WORKAROUND: Add a doc before the main transaction so it doesn't start at sequence 0
        Transaction t(db);
        store->set(slice("a"), slice("A"), t);
    }

    unique_ptr<DataFile> aliased_db { newDatabase(db->filePath()) };
    REQUIRE(aliased_db->defaultKeyStore().get(slice("a")).body() == alloc_slice("A"));

    {
        Transaction t(db);
        Document doc(slice("doc"));
        doc.setMeta(slice("m-e-t-a"));
        doc.setBody(slice("THIS IS THE BODY"));
        store->write(doc, t);

        REQUIRE(doc.sequence() == 2);
        REQUIRE(store->lastSequence() == 2);
        auto doc_alias = store->get(doc.sequence());
        REQUIRE(doc_alias.key() == doc.key());
        REQUIRE(doc_alias.meta() == doc.meta());
        REQUIRE(doc_alias.body() == doc.body());

        doc_alias.setBody(slice("NU BODY"));
        store->write(doc_alias, t);

        REQUIRE(store->read(doc));
        REQUIRE(doc.sequence() == 3);
        REQUIRE(doc.meta() == doc_alias.meta());
        REQUIRE(doc.body() == doc_alias.body());

        // Doc shouldn't exist outside transaction yet:
        REQUIRE(aliased_db->defaultKeyStore().get(slice("doc")).sequence() == 0);
    }

    REQUIRE(store->get(slice("doc")).sequence() == 3);
    REQUIRE(aliased_db->defaultKeyStore().get(slice("doc")).sequence() == 3);
}

static void createNumberedDocs(KeyStore *store) {
    Transaction t(store->dataFile());
    for (int i = 1; i <= 100; i++) {
        string docID = stringWithFormat("doc-%03d", i);
        sequence seq = store->set(slice(docID), litecore::slice::null, slice(docID), t);
        REQUIRE(seq == (sequence)i);
        REQUIRE(store->get(slice(docID)).body() == alloc_slice(docID));
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile EnumerateDocs", "[DataFile]") {
    {
        Log("Enumerate empty db");
        int i = 0;
        DocEnumerator e(*store);
        for (; e.next(); ++i) {
            FAIL("Shouldn't have found any docs");
        }
        REQUIRE_FALSE(e);
    }

    createNumberedDocs(store);

    for (int metaOnly=0; metaOnly <= 1; ++metaOnly) {
        Log("Enumerate over all docs (metaOnly=%d)", metaOnly);
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = metaOnly ? kMetaOnly : kDefaultContent;

        {
            int i = 1;
            DocEnumerator e(*store, slice::null, slice::null, opts);
            for (; e.next(); ++i) {
                string expectedDocID = stringWithFormat("doc-%03d", i);
                REQUIRE(e->key() == alloc_slice(expectedDocID));
                REQUIRE(e->sequence() == (sequence)i);
                REQUIRE(e->bodySize() > 0); // even metaOnly should set the body size
                if (isForestDB())
                    REQUIRE(e->offset() > 0);
            }
            REQUIRE(i == 101);
            REQUIRE_FALSE(e);
        }

        Log("Enumerate over range of docs:");
        int i = 24;
        for (DocEnumerator e(*store, slice("doc-024"), slice("doc-029"), opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("doc-%03d", i);
            REQUIRE(e->key() == alloc_slice(expectedDocID));
            REQUIRE(e->sequence() == (sequence)i);
            REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
            if (isForestDB())
                REQUIRE(e->offset() > 0);
        }
        REQUIRE(i == 30);

        Log("Enumerate over range of docs without inclusive:");
        opts.inclusiveStart = opts.inclusiveEnd = false;
        i = 25;
        for (DocEnumerator e(*store, slice("doc-024"), slice("doc-029"), opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("doc-%03d", i);
            REQUIRE(e->key() == alloc_slice(expectedDocID));
            REQUIRE(e->sequence() == (sequence)i);
            REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
            if (isForestDB())
                REQUIRE(e->offset() > 0);
        }
        REQUIRE(i == 29);
        opts.inclusiveStart = opts.inclusiveEnd = true;

        Log("Enumerate over vector of docs:");
        i = 0;
        vector<string> docIDs;
        docIDs.push_back("doc-005");
        docIDs.push_back("doc-029");
        docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
        docIDs.push_back("doc-028");
        docIDs.push_back("doc-098");
        docIDs.push_back("doc-100");
        docIDs.push_back("doc-105"); // doesn't exist!
        for (DocEnumerator e(*store, docIDs, opts); e.next(); ++i) {
            Log("key = %s", e->key().cString());
            REQUIRE((string)e->key() == docIDs[i]);
            REQUIRE(e->exists() == i < 6);
            if (i < 6) {
                REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
                if (isForestDB())
                    REQUIRE(e->offset() > 0);
            }
        }
        REQUIRE(i == 7);
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile EnumerateDocsDescending", "[DataFile]") {
    auto opts = DocEnumerator::Options::kDefault;
    opts.descending = true;

    createNumberedDocs(store);

    SECTION("Enumerate over all docs, descending:") {
        int i = 100;
        for (DocEnumerator e(*store, slice::null, slice::null, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 0);
    }

    SECTION("Enumerate over range of docs from max, descending:") {
        int i = 100;
        for (DocEnumerator e(*store, slice::null, slice("doc-090"), opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 89);
    }

    SECTION("Enumerate over range of docs to min, descending:") {
        int i = 10;
        for (DocEnumerator e(*store, slice("doc-010"), slice::null, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 0);
    }

    SECTION("Enumerate over range of docs, descending:") {
        int i = 29;
        for (DocEnumerator e(*store, slice("doc-029"), slice("doc-024"), opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 23);
    }

    SECTION("Enumerate over range of docs, descending, max key doesn't exist:") {
        int i = 29;
        for (DocEnumerator e(*store, slice("doc-029b"), slice("doc-024"), opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 23);
    }

    SECTION("Enumerate over range of docs without inclusive, descending:") {
        auto optsExcl = opts;
        optsExcl.inclusiveStart = optsExcl.inclusiveEnd = false;
        int i = 28;
        for (DocEnumerator e(*store, slice("doc-029"), slice("doc-024"), optsExcl); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 24);
    }

    SECTION("Enumerate over vector of docs, descending:") {
        vector<string> docIDs;
        docIDs.push_back("doc-005");
        docIDs.push_back("doc-029");
        docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
        docIDs.push_back("doc-028");
        docIDs.push_back("doc-098");
        docIDs.push_back("doc-100");
        docIDs.push_back("doc-105");
        int i = (int)docIDs.size() - 1;
        for (DocEnumerator e(*store, docIDs, opts); e.next(); --i) {
            Log("key = %s", e->key().cString());
            REQUIRE((string)e->key() == docIDs[i]);
        }
        REQUIRE(i == -1);
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile AbortTransaction", "[DataFile]") {
    // Initial document:
    {
        Transaction t(db);
        store->set(slice("a"), slice("A"), t);
    }
    {
        Transaction t(db);
        store->set(slice("x"), slice("X"), t);
        store->set(slice("a"), slice("Z"), t);
        REQUIRE(store->get(slice("a")).body() == alloc_slice("Z"));
        REQUIRE(store->get(slice("a")).body() == alloc_slice("Z"));
        t.abort();
    }
    REQUIRE(store->get(slice("a")).body() == alloc_slice("A"));
    REQUIRE(store->get(slice("x")).sequence() == 0);
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
            store->set(slice(docID), slice::null, slice("some document content goes here"), trans);
        }
    }

    int i = 0;
    for (DocEnumerator iter(db2->defaultKeyStore()); iter.next(); ) {
        slice key = (*iter).key();
        //Log("key = %s", key);
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
        store->set(key, slice("A"), t);
    }
    REQUIRE(store->lastSequence() == 1);
    REQUIRE(db->purgeCount() == 0);
    {
        Transaction t(db);
        store->del(key, t);
    }
    Document doc = store->get(key);
    REQUIRE_FALSE(doc.exists());
    REQUIRE(store->lastSequence() == 2);
    REQUIRE(db->purgeCount() == 0); // doesn't increment until after compaction
    db->compact();
    REQUIRE(db->purgeCount() == 1);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile DeleteDoc", "[DataFile]") {
    slice key("a");
    {
        Transaction t(db);
        store->set(key, slice("A"), t);
    }

    {
        Transaction t(db);
        Document doc = store->get(key);
        store->del(doc, t);
    }

    Document doc = store->get(key);
    //    REQUIRE(doc.deleted());
    REQUIRE_FALSE(doc.exists());
    
    REQUIRE(db->purgeCount() == 0); // doesn't increment until after compaction
    db->compact();
    REQUIRE(db->purgeCount() == 1);
}


// Tests workaround for ForestDB bug MB-18753
N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile DeleteDocAndReopen", "[DataFile]") {
    slice key("a");
    {
        Transaction t(db);
        store->set(key, slice("A"), t);
    }

    {
        Transaction t(db);
        Document doc = store->get(key);
        store->del(doc, t);
    }

    Document doc = store->get(key);
    //    REQUIRE(doc.deleted());
    REQUIRE_FALSE(doc.exists());

    reopenDatabase();

    Document doc2 = store->get(key);
    //    REQUIRE(doc2.deleted());
    REQUIRE_FALSE(doc2.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreInfo", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    REQUIRE(s.lastSequence() == 0);
    REQUIRE(s.name() == string("store"));

    REQUIRE(s.documentCount() == 0);
    REQUIRE(s.lastSequence() == 0);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreWrite", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    {
        Transaction t(db);
        s.set(key, slice("value"), t);
    }
    REQUIRE(s.lastSequence() == 1);
    Document doc = s.get(key);
    REQUIRE(doc.key() == key);
    REQUIRE(doc.body() == slice("value"));

    Document doc2 = store->get(key);
    REQUIRE_FALSE(doc2.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreDelete", "[DataFile]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
//    {
//        Transaction t(db);
//        t(s).set(key, slice("value"));
//    }
    s.erase();
    REQUIRE(s.lastSequence() == 0);
    Document doc = s.get(key);
    REQUIRE_FALSE(doc.exists());
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreAfterClose", "[DataFile][!throws]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    db->close();
    try {
        Log("NOTE: Expecting an invalid-handle exception to be thrown");
        error::sWarnOnError = false;
        Document doc = s.get(key);
    } catch (std::runtime_error &x) {
        error::sWarnOnError = true;
        error e = error::convertRuntimeError(x).standardized();
        REQUIRE(e.code == (int)error::NotOpen);
        return;
    }
    error::sWarnOnError = true;
    FAIL("Should have thrown exception");
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile ReadOnly", "[DataFile][!throws]") {
    {
        Transaction t(db);
        store->set(slice("key"), slice("value"), t);
    }
    // Reopen db as read-only:
    auto options = db->options();
    options.writeable = false;
    options.create = false;
    reopenDatabase(&options);

    auto doc = store->get(slice("key"));
    REQUIRE(doc.exists());

    // Attempt to change a doc:
    int code = 0;
    try {
        Transaction t(db);
        // This is expected to throw an exception:
        Log("NOTE: Expecting a read-only exception to be thrown");
        error::sWarnOnError = false;
        store->set(slice("key"), slice("somethingelse"), t);
    } catch (std::runtime_error &x) {
        error e = error::convertRuntimeError(x).standardized();
        code = e.code;
    }
    error::sWarnOnError = true;
    REQUIRE(code == error::NotWriteable);

    // Now try to open a nonexistent db, read-only:
    code = 0;
    try {
        Log("NOTE: Expecting a no-such-file exception to be thrown");
        error::sWarnOnError = false;
        (void)newDatabase("/tmp/db_non_existent", &options);
    } catch (std::runtime_error &x) {
        error e = error::convertRuntimeError(x).standardized();
        code = e.code;
    }
    error::sWarnOnError = true;
    REQUIRE(code == error::CantOpenFile);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Compact", "[DataFile]") {
    createNumberedDocs(store);

    {
        Transaction t(db);
        for (int i = 1; i <= 100; i += 3) {
            auto docID = stringWithFormat("doc-%03d", i);
            Document doc = store->get((slice)docID);
            store->del(doc, t);
        }
    }

    unsigned numCompactCalls = 0;
    db->setOnCompact([&](bool compacting) {
        ++numCompactCalls;
    });

    db->compact();

    db->setOnCompact(nullptr);
    REQUIRE(numCompactCalls == 2u);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Encryption", "[DataFile][!throws]") {
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = DataFile::kAES256;
    options.encryptionKey = slice("12345678901234567890123456789012");
    auto dbPath = databasePath("encrypted");
    deleteDatabase(dbPath);
    {
        // Create encrypted db:
        unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
        Transaction t(*encryptedDB);
        encryptedDB->defaultKeyStore().set(slice("k"), slice::null, slice("value"), t);
    }
    {
        // Reopen with correct key:
        unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
        auto doc = encryptedDB->defaultKeyStore().get(slice("k"));
        REQUIRE(doc.body() == alloc_slice("value"));
    }
    {
        // Reopen without key:
        options.encryptionAlgorithm = DataFile::kNoEncryption;
        int code = 0;
        try {
            Log("NOTE: Expecting a can't-open-file exception to be thrown");
            error::sWarnOnError = false;
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
        } catch (std::runtime_error &x) {
            error e = error::convertRuntimeError(x).standardized();
            REQUIRE(e.domain == error::LiteCore);
            code = e.code;
        }
        error::sWarnOnError = true;
        REQUIRE(code == (int)error::NotADatabaseFile);
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Rekey", "[DataFile]") {
    auto dbPath = db->filePath();
    auto options = db->options();
    createNumberedDocs(store);

    options.encryptionAlgorithm = DataFile::kAES256;
    options.encryptionKey = alloc_slice(32);
    randomBytes(options.encryptionKey);

    db->rekey(options.encryptionAlgorithm, options.encryptionKey);

    reopenDatabase(&options);

    Document doc = store->get((slice)"doc-001");
    REQUIRE(doc.exists());
}
