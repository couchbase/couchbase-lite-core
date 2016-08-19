//
//  DataFile_Test.cc
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "DataFile.hh"
#include "DocEnumerator.hh"
#include "Error.hh"

#include "CBForestTest.hh"

using namespace cbforest;
using namespace std;


class DataFileTest : public DataFileTestFixture {

void testDbInfo() {
    Assert(db->isOpen());
    Assert(!db->isCompacting());
    Assert(!DataFile::isAnyCompacting());
    AssertEqual(db->purgeCount(), 0ull);
    AssertEqual(&store->dataFile(), db);
    AssertEqual(store->documentCount(), 0ull);
    AssertEqual(store->lastSequence(), 0ull);
}

void testCreateDoc() {
    alloc_slice key("key");
    {
        Transaction t(db);
        store->set(key, slice("value"), t);
    }
    AssertEqual(store->lastSequence(), 1ull);
    Document doc = db->defaultKeyStore().get(key);
    Assert(doc.key() == key);
    Assert(doc.body() == slice("value"));
}

void testSaveDocs() {
    {
        //WORKAROUND: Add a doc before the main transaction so it doesn't start at sequence 0
        Transaction t(db);
        store->set(slice("a"), slice("A"), t);
    }

    unique_ptr<DataFile> aliased_db { newDatabase(db->filename()) };
    AssertEqual(aliased_db->defaultKeyStore().get(slice("a")).body(), alloc_slice("A"));

    {
        Transaction t(db);
        Document doc(slice("doc"));
        doc.setMeta(slice("m-e-t-a"));
        doc.setBody(slice("THIS IS THE BODY"));
        store->write(doc, t);

        AssertEqual(doc.sequence(), 2ull);
        AssertEqual(store->lastSequence(), 2ull);
        auto doc_alias = store->get(doc.sequence());
        AssertEqual(doc_alias.key(), doc.key());
        AssertEqual(doc_alias.meta(), doc.meta());
        AssertEqual(doc_alias.body(), doc.body());

        doc_alias.setBody(slice("NU BODY"));
        store->write(doc_alias, t);

        Assert(store->read(doc));
        AssertEqual(doc.sequence(), 3ull);
        Assert(doc.meta() == doc_alias.meta());
        Assert(doc.body() == doc_alias.body());

        // Doc shouldn't exist outside transaction yet:
        AssertEqual(aliased_db->defaultKeyStore().get(slice("doc")).sequence(), 0ull);
    }

    AssertEqual(store->get(slice("doc")).sequence(), 3ull);
    AssertEqual(aliased_db->defaultKeyStore().get(slice("doc")).sequence(), 3ull);
}

void createNumberedDocs() {
    Transaction t(db);
    for (int i = 1; i <= 100; i++) {
        string docID = stringWithFormat("doc-%03d", i);
        sequence seq = store->set(slice(docID), cbforest::slice::null, slice(docID), t);
        AssertEqual(seq, (sequence)i);
        AssertEqual(store->get(slice(docID)).body(), alloc_slice(docID));
    }
}

void testEnumerateDocs() {
    {
        Log("Enumerate empty db");
        int i = 0;
        DocEnumerator e(*store);
        for (; e.next(); ++i) {
            AssertionFailed("Shouldn't have found any docs");
        }
        Assert(!e);
    }

    createNumberedDocs();

    for (int metaOnly=0; metaOnly <= 1; ++metaOnly) {
        Log("Enumerate over all docs (metaOnly=%d)", metaOnly);
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = metaOnly ? kMetaOnly : kDefaultContent;

        {
            int i = 1;
            DocEnumerator e(*store, slice::null, slice::null, opts);
            for (; e.next(); ++i) {
                string expectedDocID = stringWithFormat("doc-%03d", i);
                AssertEqual(e->key(), alloc_slice(expectedDocID));
                AssertEqual(e->sequence(), (sequence)i);
                Assert(e->bodySize() > 0); // even metaOnly should set the body size
                if (isForestDB())
                    Assert(e->offset() > 0);
            }
            AssertEqual(i, 101);
            Assert(!e);
        }

        Log("Enumerate over range of docs:");
        int i = 24;
        for (DocEnumerator e(*store, slice("doc-024"), slice("doc-029"), opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("doc-%03d", i);
            AssertEqual(e->key(), alloc_slice(expectedDocID));
            AssertEqual(e->sequence(), (sequence)i);
            Assert(e->bodySize() > 0); // even metaOnly should set the body length
            if (isForestDB())
                Assert(e->offset() > 0);
        }
        AssertEqual(i, 30);

        Log("Enumerate over range of docs without inclusive:");
        opts.inclusiveStart = opts.inclusiveEnd = false;
        i = 25;
        for (DocEnumerator e(*store, slice("doc-024"), slice("doc-029"), opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("doc-%03d", i);
            AssertEqual(e->key(), alloc_slice(expectedDocID));
            AssertEqual(e->sequence(), (sequence)i);
            Assert(e->bodySize() > 0); // even metaOnly should set the body length
            if (isForestDB())
                Assert(e->offset() > 0);
        }
        AssertEqual(i, 29);
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
            AssertEqual((string)e->key(), docIDs[i]);
            AssertEqual(e->exists(), i < 6);
            if (i < 6) {
                Assert(e->bodySize() > 0); // even metaOnly should set the body length
                if (isForestDB())
                    Assert(e->offset() > 0);
            }
        }
        AssertEqual(i, 7);
    }
}

void testEnumerateDocsDescending() {
    auto opts = DocEnumerator::Options::kDefault;
    opts.descending = true;

    createNumberedDocs();
    Log("Enumerate over all docs, descending:");
    int i = 100;
    for (DocEnumerator e(*store, slice::null, slice::null, opts); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 0);

    Log("Enumerate over range of docs from max, descending:");
    i = 100;
    for (DocEnumerator e(*store, slice::null, slice("doc-090"), opts); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 89);

    Log("Enumerate over range of docs to min, descending:");
    i = 10;
    for (DocEnumerator e(*store, slice("doc-010"), slice::null, opts); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 0);

    Log("Enumerate over range of docs, descending:");
    i = 29;
    for (DocEnumerator e(*store, slice("doc-029"), slice("doc-024"), opts); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 23);

    Log("Enumerate over range of docs, descending, max key doesn't exist:");
    i = 29;
    for (DocEnumerator e(*store, slice("doc-029b"), slice("doc-024"), opts); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 23);

    Log("Enumerate over range of docs without inclusive, descending:");
    auto optsExcl = opts;
    optsExcl.inclusiveStart = optsExcl.inclusiveEnd = false;
    i = 28;
    for (DocEnumerator e(*store, slice("doc-029"), slice("doc-024"), optsExcl); e.next(); --i) {
        alloc_slice expectedDocID(stringWithFormat("doc-%03d", i));
        AssertEqual(e->key(), expectedDocID);
        AssertEqual(e->sequence(), (sequence)i);
    }
    AssertEqual(i, 24);

    Log("Enumerate over vector of docs, descending:");
    vector<string> docIDs;
    docIDs.push_back("doc-005");
    docIDs.push_back("doc-029");
    docIDs.push_back("doc-023"); // out of order! (check for random-access fdb_seek)
    docIDs.push_back("doc-028");
    docIDs.push_back("doc-098");
    docIDs.push_back("doc-100");
    docIDs.push_back("doc-105");
    i = (int)docIDs.size() - 1;
    for (DocEnumerator e(*store, docIDs, opts); e.next(); --i) {
        Log("key = %s", e->key().cString());
        Assert((string)e->key() == docIDs[i]);
    }
    AssertEqual(i, -1);
}

void testAbortTransaction() {
    // Initial document:
    {
        Transaction t(db);
        store->set(slice("a"), slice("A"), t);
    }
    {
        Transaction t(db);
        store->set(slice("x"), slice("X"), t);
        store->set(slice("a"), slice("Z"), t);
        AssertEqual(store->get(slice("a")).body(), alloc_slice("Z"));
        AssertEqual(store->get(slice("a")).body(), alloc_slice("Z"));
        t.abort();
    }
    AssertEqual(store->get(slice("a")).body(), alloc_slice("A"));
    AssertEqual(store->get(slice("x")).sequence(), 0ull);
}


// Test for MB-12287
void testTransactionsThenIterate() {
    unique_ptr<DataFile> db2 { newDatabase(db->filename()) };

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
        AssertEqual(key, slice(stringWithFormat("%03lu.%03lu", (unsigned long)t, (unsigned long)d)));
        i++;
    }
}

void testDeleteKey() {
    slice key("a");
    {
        Transaction t(db);
        store->set(key, slice("A"), t);
    }
    AssertEqual(store->lastSequence(), 1ull);
    AssertEqual(db->purgeCount(), 0ull);
    {
        Transaction t(db);
        store->del(key, t);
    }
    Document doc = store->get(key);
    Assert(!doc.exists());
    AssertEqual(store->lastSequence(), 2ull);
    AssertEqual(db->purgeCount(), 0ull); // doesn't increment until after compaction
    db->compact();
    AssertEqual(db->purgeCount(), 1ull);
}

void testDeleteDoc() {
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
    //    Assert(doc.deleted());
    Assert(!doc.exists());
    
    AssertEqual(db->purgeCount(), 0ull); // doesn't increment until after compaction
    db->compact();
    AssertEqual(db->purgeCount(), 1ull);
}

// Tests workaround for ForestDB bug MB-18753
void testDeleteDocAndReopen() {
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
    //    Assert(doc.deleted());
    Assert(!doc.exists());

    reopenDatabase();

    Document doc2 = store->get(key);
    //    Assert(doc2.deleted());
    Assert(!doc2.exists());
}

void testKeyStoreInfo() {
    KeyStore &s = db->getKeyStore("store");
    AssertEqual(s.lastSequence(), 0ull);
    AssertEqual(s.name(), string("store"));

    AssertEqual(s.documentCount(), 0ull);
    AssertEqual(s.lastSequence(), 0ull);
}

void testKeyStoreWrite() {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    {
        Transaction t(db);
        s.set(key, slice("value"), t);
    }
    AssertEqual(s.lastSequence(), 1ull);
    Document doc = s.get(key);
    Assert(doc.key() == key);
    Assert(doc.body() == slice("value"));

    Document doc2 = store->get(key);
    Assert(!doc2.exists());
}

void testKeyStoreDelete() {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
//    {
//        Transaction t(db);
//        t(s).set(key, slice("value"));
//    }
    s.erase();
    AssertEqual(s.lastSequence(), 0ull);
    Document doc = s.get(key);
    Assert(!doc.exists());
}

void testKeyStoreAfterClose() {
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
        AssertEqual(e.code, (int)error::NotOpen);
        return;
    }
    error::sWarnOnError = true;
    Assert(false); // should not reach here
}

void testReadOnly() {
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
    Assert(doc.exists());

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
    AssertEqual(code, (int)error::NotWriteable);

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
    AssertEqual(code, (int)error::CantOpenFile);
}

void testCompact() {
    createNumberedDocs();

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
    AssertEqual(numCompactCalls, 2u);
}


void testEncryption() {
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = DataFile::kAES256;
    options.encryptionKey = slice("12345678901234567890123456789012");
    std::string dbPath = kTestDir "encrypted.db";
    DataFile::deleteDataFile(dbPath);
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
        AssertEqual(doc.body(), alloc_slice("value"));
    }
    {
        // Reopen without key:
        options.encryptionAlgorithm = DataFile::kNoEncryption;
        bool caughtException = false;
        try {
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
        } catch (const error &e) {
            error ee = e.standardized();
            AssertEqual(ee.domain, error::CBForest);
            AssertEqual(ee.code, (int)error::NotADatabaseFile);
            caughtException = true;
        }
        Assert(caughtException);
    }
}


void testRekey() {
    if (!isForestDB()) {
        Log("Skipping rekey test: not supported for this DB type"); //TODO: rekey for SQLite
        return;
    }

    auto dbPath = db->filename();
    auto options = db->options();
    createNumberedDocs();

    options.encryptionAlgorithm = DataFile::kAES256;
    options.encryptionKey = alloc_slice(32);
    randomBytes(options.encryptionKey);

    db->rekey(options.encryptionAlgorithm, options.encryptionKey);

    reopenDatabase(&options);

    Document doc = store->get((slice)"doc-001");
    Assert(doc.exists());
}



    CPPUNIT_TEST_SUITE( DataFileTest );
    CPPUNIT_TEST( testDbInfo );
    CPPUNIT_TEST( testCreateDoc );
    CPPUNIT_TEST( testSaveDocs );
    CPPUNIT_TEST( testEnumerateDocs );
    CPPUNIT_TEST( testEnumerateDocsDescending );
    CPPUNIT_TEST( testAbortTransaction );
    CPPUNIT_TEST( testTransactionsThenIterate );
    CPPUNIT_TEST( testDeleteKey );
    CPPUNIT_TEST( testDeleteDoc );
    CPPUNIT_TEST( testDeleteDocAndReopen );
    CPPUNIT_TEST( testKeyStoreInfo );
    CPPUNIT_TEST( testKeyStoreWrite );
    CPPUNIT_TEST( testKeyStoreDelete );
    CPPUNIT_TEST( testKeyStoreAfterClose );
    CPPUNIT_TEST( testReadOnly );
    CPPUNIT_TEST( testCompact );
    CPPUNIT_TEST( testEncryption );
    CPPUNIT_TEST( testRekey );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DataFileTest);


class DataFileSQLiteTest : public DataFileTest {
    
    virtual bool isForestDB() const             {return false;}

    CPPUNIT_TEST_SUB_SUITE( DataFileSQLiteTest, DataFileTest );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DataFileSQLiteTest);
