//
//  DataFile_Test.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "DataFile.hh"
#include "RecordEnumerator.hh"
#include "Query.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Fleece.hh"
#include "Benchmark.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DbInfo", "[DataFile]") {
    REQUIRE(db->isOpen());
    REQUIRE_FALSE(db->isCompacting());
    REQUIRE_FALSE(DataFile::isAnyCompacting());
    REQUIRE(db->purgeCount() == 0);
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
        rec.setMeta("m-e-t-a"_sl);
        rec.setBody("THIS IS THE BODY"_sl);
        store->write(rec, t);

        REQUIRE(rec.sequence() == 2);
        REQUIRE(store->lastSequence() == 2);
        auto doc_alias = store->get(rec.sequence());
        REQUIRE(doc_alias.key() == rec.key());
        REQUIRE(doc_alias.meta() == rec.meta());
        REQUIRE(doc_alias.body() == rec.body());

        doc_alias.setBody("NU BODY"_sl);
        store->write(doc_alias, t);

        REQUIRE(store->read(rec));
        REQUIRE(rec.sequence() == 3);
        REQUIRE(rec.meta() == doc_alias.meta());
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
        sequence seq = store->set(slice(docID), litecore::nullslice, slice(docID), t).seq;
        REQUIRE(seq == (sequence)i);
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
            RecordEnumerator e(*store, nullslice, nullslice, opts);
            for (; e.next(); ++i) {
                string expectedDocID = stringWithFormat("rec-%03d", i);
                REQUIRE(e->key() == alloc_slice(expectedDocID));
                REQUIRE(e->sequence() == (sequence)i);
                REQUIRE(e->bodySize() > 0); // even metaOnly should set the body size
                if (store->capabilities().getByOffset)
                    REQUIRE(e->offset() > 0);
            }
            REQUIRE(i == 101);
            REQUIRE_FALSE(e);
        }

        INFO("Enumerate over range of docs:");
        int i = 24;
        for (RecordEnumerator e(*store, "rec-024"_sl, "rec-029"_sl, opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("rec-%03d", i);
            REQUIRE(e->key() == alloc_slice(expectedDocID));
            REQUIRE(e->sequence() == (sequence)i);
            REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
            if (store->capabilities().getByOffset)
                REQUIRE(e->offset() > 0);
        }
        REQUIRE(i == 30);

        INFO("Enumerate over range of docs without inclusive:");
        opts.inclusiveStart = opts.inclusiveEnd = false;
        i = 25;
        for (RecordEnumerator e(*store, "rec-024"_sl, "rec-029"_sl, opts); e.next(); ++i) {
            string expectedDocID = stringWithFormat("rec-%03d", i);
            REQUIRE(e->key() == alloc_slice(expectedDocID));
            REQUIRE(e->sequence() == (sequence)i);
            REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
            if (store->capabilities().getByOffset)
                REQUIRE(e->offset() > 0);
        }
        REQUIRE(i == 29);
        opts.inclusiveStart = opts.inclusiveEnd = true;

        INFO("Enumerate over vector of docs:");
        i = 0;
        vector<string> docIDs;
        docIDs.push_back("rec-005");
        docIDs.push_back("rec-029");
        docIDs.push_back("rec-023"); // out of order! (check for random-access fdb_seek)
        docIDs.push_back("rec-028");
        docIDs.push_back("rec-098");
        docIDs.push_back("rec-100");
        docIDs.push_back("rec-105"); // doesn't exist!
        for (RecordEnumerator e(*store, docIDs, opts); e.next(); ++i) {
            INFO("key = " << e->key());
            REQUIRE((string)e->key() == docIDs[i]);
            REQUIRE(e->exists() == i < 6);
            if (i < 6) {
                REQUIRE(e->bodySize() > 0); // even metaOnly should set the body length
                if (store->capabilities().getByOffset)
                    REQUIRE(e->offset() > 0);
            }
        }
        REQUIRE(i == 7);
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile EnumerateDocsDescending", "[DataFile]") {
    RecordEnumerator::Options opts;
    opts.descending = true;

    createNumberedDocs(store);

    SECTION("Enumerate over all docs, descending:") {
        int i = 100;
        for (RecordEnumerator e(*store, nullslice, nullslice, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 0);
    }

    SECTION("Enumerate over range of docs from max, descending:") {
        int i = 100;
        for (RecordEnumerator e(*store, nullslice, "rec-090"_sl, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 89);
    }

    SECTION("Enumerate over range of docs to min, descending:") {
        int i = 10;
        for (RecordEnumerator e(*store, "rec-010"_sl, nullslice, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 0);
    }

    SECTION("Enumerate over range of docs, descending:") {
        int i = 29;
        for (RecordEnumerator e(*store, "rec-029"_sl, "rec-024"_sl, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 23);
    }

    SECTION("Enumerate over range of docs, descending, max key doesn't exist:") {
        int i = 29;
        for (RecordEnumerator e(*store, "rec-029b"_sl, "rec-024"_sl, opts); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 23);
    }

    SECTION("Enumerate over range of docs without inclusive, descending:") {
        auto optsExcl = opts;
        optsExcl.inclusiveStart = optsExcl.inclusiveEnd = false;
        int i = 28;
        for (RecordEnumerator e(*store, "rec-029"_sl, "rec-024"_sl, optsExcl); e.next(); --i) {
            alloc_slice expectedDocID(stringWithFormat("rec-%03d", i));
            REQUIRE(e->key() == expectedDocID);
            REQUIRE(e->sequence() == (sequence)i);
        }
        REQUIRE(i == 24);
    }

    SECTION("Enumerate over vector of docs, descending:") {
        vector<string> docIDs;
        docIDs.push_back("rec-005");
        docIDs.push_back("rec-029");
        docIDs.push_back("rec-023"); // out of order! (check for random-access fdb_seek)
        docIDs.push_back("rec-028");
        docIDs.push_back("rec-098");
        docIDs.push_back("rec-100");
        docIDs.push_back("rec-105");
        int i = (int)docIDs.size() - 1;
        for (RecordEnumerator e(*store, docIDs, opts); e.next(); --i) {
            INFO("key = " << e->key());
            REQUIRE((string)e->key() == docIDs[i]);
        }
        REQUIRE(i == -1);
    }
}


TEST_CASE_METHOD(DataFileTestFixture, "DataFile EnumerateDocsQuery", "[DataFile][Query]") {
    // Write 100 docs with Fleece bodies of the form {"num":n} where n is the rec #
    {
        Transaction t(store->dataFile());
        for (int i = 1; i <= 100; i++) {
            string docID = stringWithFormat("rec-%03d", i);

            fleece::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("num");
            enc.writeInt(i);
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();

            sequence seq = store->set(slice(docID), litecore::nullslice, body, t).seq;
            REQUIRE(seq == (sequence)i);
        }
        t.commit();
    }

    unique_ptr<Query> query{ store->compileQuery("{\"$and\": [{\"num\": {\"$gte\": 30}}, {\"num\": {\"$lte\": 40}}]}"_sl, nullslice) };

    // Use a (SQL) query based on the Fleece "num" property:
    for (int pass = 0; pass < 2; ++pass) {
        Stopwatch st;
        int i = 30;
        for (QueryEnumerator e(query.get()); e.next(); ++i) {
            string expectedDocID = stringWithFormat("rec-%03d", i);
            REQUIRE(e.recordID() == alloc_slice(expectedDocID));
            REQUIRE(e.sequence() == (sequence)i);
        }
        st.printReport("Query of $.num", i, "row");
        REQUIRE(i == 41);

        // Add an index after the first pass:
        if (pass == 0) {
            Stopwatch st2;
            store->createIndex("$.num");
            st2.printReport("Index on $.num", 1, "index");
        }
    }
}


TEST_CASE_METHOD(DataFileTestFixture, "DataFile FullTextQuery", "[DataFile][Query]") {
    store->createIndex("$.sentence", KeyStore::kFullTextIndex);

    // Add some text to the database:
    static const char* strings[] = {"FTS5 is an SQLite virtual table module that provides full-text search functionality to database applications.",
        "In their most elementary form, full-text search engines allow the user to efficiently search a large collection of documents for the subset that contain one or more instances of a search term.",
        "The search functionality provided to world wide web users by Google is, among other things, a full-text search engine, as it allows users to search for all documents on the web that contain, for example, the term \"fts5\".",
        "To use FTS5, the user creates an FTS5 virtual table with one or more columns."};
    {
        Transaction t(store->dataFile());
        for (int i = 0; i < sizeof(strings)/sizeof(strings[0]); i++) {
            string docID = stringWithFormat("rec-%03d", i);

            fleece::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("sentence");
            enc.writeString(strings[i]);
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();

            store->set(slice(docID), litecore::nullslice, body, t);
        }
        t.commit();
    }

    unique_ptr<Query> query{ store->compileQuery(enquotify("{`sentence`: {`$match`: `search`}}"), nullslice) };
    REQUIRE(query != nullptr);
    unsigned rows = 0;
    for (QueryEnumerator e(query.get()); e.next(); ) {
        Log("key = %s", e.recordID().cString());
        CHECK(e.fullTextTerms().size() > 0);
        for (auto term : e.fullTextTerms()) {
            CHECK(e.recordID() == (slice)stringWithFormat("rec-%03d", rows));
            auto word = string(strings[rows] + term.start, term.length);
            CHECK(word == "search");
        }
        ++rows;
    }
    CHECK(rows == 3);
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
            store->set(slice(docID), nullslice, "some record content goes here"_sl, trans);
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
    REQUIRE(db->purgeCount() == 0);
    {
        Transaction t(db);
        store->del(key, t);
        t.commit();
    }
    Record rec = store->get(key);
    REQUIRE_FALSE(rec.exists());
    REQUIRE(store->lastSequence() == 2);
    REQUIRE(db->purgeCount() == 0); // doesn't increment until after compaction
    db->compact();
    REQUIRE(db->purgeCount() == 1);
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
    
    REQUIRE(db->purgeCount() == 0); // doesn't increment until after compaction
    db->compact();
    REQUIRE(db->purgeCount() == 1);
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


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreGetByOffset", "[DataFile]") {
    auto cap = KeyStore::Capabilities::defaults;
    cap.getByOffset = cap.sequences = true;
    KeyStore &s = db->getKeyStore("store", cap);
    alloc_slice key("key");
    docOffset offset1;
    // Create a rec:
    {
        Transaction t(db);
        Record rec(key);
        rec.setBody("value1"_sl);
        s.write(rec, t);
        offset1 = rec.offset();
        CHECK(offset1 > 0);
        t.commit();
    }
    // Get it by offset:
    Record doc1 = s.getByOffsetNoErrors(offset1, 1);
    REQUIRE(doc1.key() == key);
    REQUIRE(doc1.body() == "value1"_sl);
    REQUIRE(doc1.sequence() == 1);
    REQUIRE(doc1.offset() == offset1);

    // Update rec:
    docOffset offset2;
    {
        Transaction t(db);
        auto result = s.set(key, "value2"_sl, t);
        offset2 = result.off;
        CHECK(offset2 > 0);
        CHECK(result.seq == 2);
        t.commit();
    }
    // Get it by offset:
    Record doc2 = s.getByOffsetNoErrors(offset2, 2);
    REQUIRE(doc2.key() == key);
    REQUIRE(doc2.body() == "value2"_sl);
    REQUIRE(doc2.sequence() == 2);
    REQUIRE(doc2.offset() == offset2);

    // Get old version (Seq 1) by offset:
    Record doc1again = s.getByOffsetNoErrors(offset1, 1);
    REQUIRE(doc1again.key() == key);
    REQUIRE(doc1again.body() == "value1"_sl);
    REQUIRE(doc1again.sequence() == 1);
    REQUIRE(doc1again.offset() == offset1);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile KeyStoreAfterClose", "[DataFile][!throws]") {
    KeyStore &s = db->getKeyStore("store");
    alloc_slice key("key");
    db->close();
    try {
        INFO("NOTE: Expecting an invalid-handle exception to be thrown");
        error::sWarnOnError = false;
        Record rec = s.get(key);
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
    int code = 0;
    try {
        Transaction t(db);
        // This is expected to throw an exception:
        INFO("NOTE: Expecting a read-only exception to be thrown");
        error::sWarnOnError = false;
        store->set("key"_sl, "somethingelse"_sl, t);
        t.commit();
    } catch (std::runtime_error &x) {
        error e = error::convertRuntimeError(x).standardized();
        code = e.code;
    }
    error::sWarnOnError = true;
    REQUIRE(code == error::NotWriteable);

    // Now try to open a nonexistent db, read-only:
    code = 0;
    try {
        INFO("NOTE: Expecting a no-such-file exception to be thrown");
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
            auto docID = stringWithFormat("rec-%03d", i);
            Record rec = store->get((slice)docID);
            store->del(rec, t);
        }
        t.commit();
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
    if (!factory().encryptionEnabled(kAES256)) {
        cerr << "Skipping encryption test; not enabled for " << factory().cname() << "\n";
        return;
    }
    DataFile::Options options = db->options();
    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = "12345678901234567890123456789012"_sl;
    auto dbPath = databasePath("encrypted");
    deleteDatabase(dbPath);
    try {
        {
            // Create encrypted db:
            unique_ptr<DataFile> encryptedDB { newDatabase(dbPath, &options) };
            Transaction t(*encryptedDB);
            encryptedDB->defaultKeyStore().set("k"_sl, nullslice, "value"_sl, t);
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
            int code = 0;
            try {
                INFO("NOTE: Expecting a can't-open-file exception to be thrown");
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
    } catch (...) {
        deleteDatabase(dbPath);
        throw;
    }
    deleteDatabase(dbPath);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "DataFile Rekey", "[DataFile]") {
    if (!factory().encryptionEnabled(kAES256)) {
        cerr << "Skipping rekeying test; encryption not enabled for " << factory().cname() << "\n";
        return;
    }

    auto dbPath = db->filePath();
    auto options = db->options();
    createNumberedDocs(store);

    options.encryptionAlgorithm = kAES256;
    options.encryptionKey = alloc_slice(32);
    randomBytes(options.encryptionKey);

    db->rekey(options.encryptionAlgorithm, options.encryptionKey);

    reopenDatabase(&options);

    Record rec = store->get((slice)"rec-001");
    REQUIRE(rec.exists());
}
