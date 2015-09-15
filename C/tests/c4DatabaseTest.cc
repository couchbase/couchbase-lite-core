//
//  c4DatabaseTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

//  Requires CppUnit <http://sourceforge.net/projects/cppunit/> which you can install using
//  'brew' or 'apt_get' or whatever.

#include <cppunit/TestCase.h>
#include <cppunit/extensions/HelperMacros.h>
#include "c4Database.h"
#include "forestdb.h"
#include "iostream"


#define Assert CPPUNIT_ASSERT
#define AssertEqual(ACTUAL, EXPECTED) CPPUNIT_ASSERT_EQUAL(EXPECTED, ACTUAL)


static bool operator== (C4Slice s1, C4Slice s2) {
    return s1.size == s2.size && memcmp(s1.buf, s2.buf, s1.size) == 0;
}

static std::ostream& operator<< (std::ostream& o, C4Slice s) {
    auto buf = (const uint8_t*)s.buf;
    for (size_t i = 0; i < s.size; i++) {
        if (buf[i] < 32 || buf[i] > 126)
            return o << "C4Slice[binary, " << s.size << " bytes]";
    }
    return o << "C4Slice[\"" << std::string((char*)s.buf, s.size) << "\"]";
}


// This helper is necessary because it ends an open transaction if an assertion fails.
// If the transaction isn't ended, the c4db_delete call in tearDown will deadlock.
class TransactionHelper {
    public:
    TransactionHelper(C4Database* db)
    :_db(NULL)
    {
        C4Error error;
        Assert(c4db_beginTransaction(db, &error));
        _db = db;
    }

    ~TransactionHelper() {
        if (_db) {
            C4Error error;
            Assert(c4db_endTransaction(_db, true, &error));
        }
    }

    private:
    C4Database* _db;
};


class C4DatabaseTest : public CppUnit::TestFixture {
    public:

    void setUp() {
        const char *dbPath = "/tmp/forest_temp.fdb";
        ::unlink(dbPath);
        C4Error error;
        db = c4db_open(c4str(dbPath), false, &error);
        Assert(db != NULL);
    }

    void tearDown() {
        C4Error error;
        Assert(c4db_delete(db, &error));
    }


    void testTransaction() {
        AssertEqual(c4db_getDocumentCount(db), 0uLL);
        Assert(!c4db_isInTransaction(db));
        C4Error(error);
        Assert(c4db_beginTransaction(db, &error));
        Assert(c4db_isInTransaction(db));
        Assert(c4db_beginTransaction(db, &error));
        Assert(c4db_isInTransaction(db));
        Assert(c4db_endTransaction(db, true, &error));
        Assert(c4db_isInTransaction(db));
        Assert(c4db_endTransaction(db, true, &error));
        Assert(!c4db_isInTransaction(db));
    }


    void testCreateRawDoc() {
        const C4Slice key = c4str("key");
        const C4Slice meta = c4str("meta");
        C4Error error;
        Assert(c4db_beginTransaction(db, &error));
        c4raw_put(db, c4str("test"), key, meta, kBody, &error);
        Assert(c4db_endTransaction(db, true, &error));

        C4RawDocument *doc = c4raw_get(db, c4str("test"), key, &error);
        Assert(doc != NULL);
        AssertEqual(doc->key, key);
        AssertEqual(doc->meta, meta);
        AssertEqual(doc->body, kBody);
        c4raw_free(doc);
    }


    void testCreateVersionedDoc() {
        // Try reading doc with mustExist=true, which should fail:
        C4Error error;
        C4Document* doc;
        doc = c4doc_get(db, kDocID, true, &error);
        Assert(!doc);
        AssertEqual(error.domain, ForestDBDomain);
        AssertEqual(error.code, (int)FDB_RESULT_KEY_NOT_FOUND);

        // Now get the doc with mustExist=false, which returns an empty doc:
        doc = c4doc_get(db, kDocID, false, &error);
        Assert(doc != NULL);
        AssertEqual(doc->flags, (C4DocumentFlags)0);
        AssertEqual(doc->docID, kDocID);
        Assert(doc->revID.buf == 0);
        Assert(doc->selectedRev.revID.buf == 0);

        {
            TransactionHelper t(db);

            Assert(c4doc_insertRevision(doc, kRevID, kBody, false, false, false, &error));

            AssertEqual(doc->revID, kRevID);
            AssertEqual(doc->selectedRev.revID, kRevID);
            AssertEqual(doc->selectedRev.flags, (C4RevisionFlags)(kRevNew | kRevLeaf));
            AssertEqual(doc->selectedRev.body, kBody);
            Assert(c4doc_save(doc, 20, &error));
            c4doc_free(doc);
        }

        // Reload the doc:
        doc = c4doc_get(db, kDocID, true, &error);
        Assert(doc != NULL);
        AssertEqual(doc->flags, kExists);
        AssertEqual(doc->docID, kDocID);
        AssertEqual(doc->revID, kRevID);
        AssertEqual(doc->selectedRev.revID, kRevID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)1);
        AssertEqual(doc->selectedRev.body, kBody);
    }


    void testCreateMultipleRevisions() {
        const C4Slice kRev2ID = C4STR("2-d00d3333");
        const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
        C4Error error;
        {
            // Add 1st revision:
            TransactionHelper t(db);
            C4Document *doc = c4doc_get(db, kDocID, false, &error);
            Assert(c4doc_insertRevision(doc, kRevID,  kBody,  false, false, false, &error));
            Assert(c4doc_save(doc, 20, &error));
            c4doc_free(doc);
        }
        {
            // Add 2nd revision:
            TransactionHelper t(db);
            C4Document *doc = c4doc_get(db, kDocID, false, &error);
            Assert(c4doc_insertRevision(doc, kRev2ID, kBody2, false, false, false, &error));
            AssertEqual(doc->selectedRev.revID, kRev2ID);
            Assert(c4doc_save(doc, 20, &error));
            c4doc_free(doc);
        }

        // Reload the doc:
        C4Document *doc = c4doc_get(db, kDocID, true, &error);
        Assert(doc != NULL);
        AssertEqual(doc->flags, kExists);
        AssertEqual(doc->docID, kDocID);
        AssertEqual(doc->revID, kRev2ID);
        AssertEqual(doc->selectedRev.revID, kRev2ID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)2);
        AssertEqual(doc->selectedRev.body, kBody2);

        // Select 1st revision:
        Assert(c4doc_selectParentRevision(doc));
        AssertEqual(doc->selectedRev.revID, kRevID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)1);
        AssertEqual(doc->selectedRev.body, kC4SliceNull);
        Assert(c4doc_loadRevisionBody(doc, &error));
        AssertEqual(doc->selectedRev.body, kBody);
        Assert(!c4doc_selectParentRevision(doc));
    }

    private:

    C4Database *db;

    static const C4Slice kDocID;
    static const C4Slice kRevID;
    static const C4Slice kBody;

    CPPUNIT_TEST_SUITE( C4DatabaseTest );
    CPPUNIT_TEST( testTransaction );
    CPPUNIT_TEST( testCreateRawDoc );
    CPPUNIT_TEST( testCreateVersionedDoc );
    CPPUNIT_TEST( testCreateMultipleRevisions );
    CPPUNIT_TEST_SUITE_END();
};


const C4Slice C4DatabaseTest::kDocID = C4STR("mydoc");
const C4Slice C4DatabaseTest::kRevID = C4STR("1-abcdef");
const C4Slice C4DatabaseTest::kBody  = C4STR("{\"name\":007}");


CPPUNIT_TEST_SUITE_REGISTRATION(C4DatabaseTest);
