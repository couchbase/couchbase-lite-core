//
//  c4DatabaseTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

//  Requires CppUnit <http://sourceforge.net/projects/cppunit/>,
//  which you can install using 'brew' or 'apt_get' or whatever.

#include "c4Test.hh"
#include "forestdb.h"


class C4DatabaseTest : public C4Test {
    public:

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

        // Get the doc by its sequence:
        doc = c4doc_getBySequence(db, 1, &error);
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
        createRev(kDocID, kRevID, kBody);
        createRev(kDocID, kRev2ID, kBody2);

        // Reload the doc:
        C4Error error;
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
        Assert(c4doc_hasRevisionBody(doc));
        Assert(c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
        AssertEqual(doc->selectedRev.body, kBody);
        Assert(!c4doc_selectParentRevision(doc));

        // Compact database:
        Assert(c4db_compact(db, &error));

        doc = c4doc_get(db, kDocID, true, &error);
        Assert(doc != NULL);
        Assert(c4doc_selectParentRevision(doc));
        AssertEqual(doc->selectedRev.revID, kRevID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)1);
        AssertEqual(doc->selectedRev.body, kC4SliceNull);
        Assert(!c4doc_hasRevisionBody(doc));
        Assert(!c4doc_loadRevisionBody(doc, &error));
    }


    void testAllDocs() {
        char docID[20];
        for (int i = 1; i < 100; i++) {
            sprintf(docID, "doc-%03d", i);
            createRev(c4str(docID), kRevID, kBody);
        }

        C4Error error;
        C4DocEnumerator* e;
        C4Document* doc;

        // No start or end ID:
        C4AllDocsOptions options = kC4DefaultAllDocsOptions;
        options.includeBodies = false;
        e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
        Assert(e);
        int i = 1;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            sprintf(docID, "doc-%03d", i);
            AssertEqual(doc->docID, c4str(docID));
            AssertEqual(doc->revID, kRevID);
            AssertEqual(doc->selectedRev.revID, kRevID);
            AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)i);
            AssertEqual(doc->selectedRev.body, kC4SliceNull);
            // Doc was loaded without its body, but it should load on demand:
            Assert(c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
            AssertEqual(doc->selectedRev.body, kBody);

            c4doc_free(doc);
            i++;
        }

        // Start and end ID:
        e = c4db_enumerateAllDocs(db, c4str("doc-007"), c4str("doc-090"), NULL, &error);
        Assert(e);
        i = 7;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            sprintf(docID, "doc-%03d", i);
            AssertEqual(doc->docID, c4str(docID));
            c4doc_free(doc);
            i++;
        }
        AssertEqual(i, 91);
    }


    void testChanges() {
        char docID[20];
        for (int i = 1; i < 100; i++) {
            sprintf(docID, "doc-%03d", i);
            createRev(c4str(docID), kRevID, kBody);
        }

        C4Error error;
        C4DocEnumerator* e;
        C4Document* doc;

        // Since start:
        C4ChangesOptions options = kC4DefaultChangesOptions;
        options.includeBodies = false;
        e = c4db_enumerateChanges(db, 0, &options, &error);
        Assert(e);
        C4SequenceNumber seq = 1;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            AssertEqual(doc->selectedRev.sequence, seq);
            sprintf(docID, "doc-%03llu", seq);
            AssertEqual(doc->docID, c4str(docID));
            c4doc_free(doc);
            seq++;
        }

        // Since 6:
        e = c4db_enumerateChanges(db, 6, &options, &error);
        Assert(e);
        seq = 7;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            AssertEqual(doc->selectedRev.sequence, seq);
            sprintf(docID, "doc-%03llu", seq);
            AssertEqual(doc->docID, c4str(docID));
            c4doc_free(doc);
            seq++;
        }
        AssertEqual(seq, 100ull);
    }


    CPPUNIT_TEST_SUITE( C4DatabaseTest );
    CPPUNIT_TEST( testTransaction );
    CPPUNIT_TEST( testCreateRawDoc );
    CPPUNIT_TEST( testCreateVersionedDoc );
    CPPUNIT_TEST( testCreateMultipleRevisions );
    CPPUNIT_TEST( testAllDocs );
    CPPUNIT_TEST( testChanges );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4DatabaseTest);


class C4EncryptedDatabaseTest : public C4DatabaseTest {
    static C4EncryptionKey sKey;
    const C4EncryptionKey* encryptionKey()  {
        sKey.algorithm = kC4EncryptionAES256;
        memcpy(sKey.bytes, "this is not a random key at all...", 32);
        return &sKey;
    }


    void testRekey() {
        testCreateRawDoc();
        
        C4Error error;
        Assert(c4db_rekey(db, NULL, &error));

        const C4Slice key = c4str("key");
        C4RawDocument *doc = c4raw_get(db, c4str("test"), key, &error);
        Assert(doc != NULL);
    }


    CPPUNIT_TEST_SUITE( C4EncryptedDatabaseTest );
    CPPUNIT_TEST( testTransaction );
    CPPUNIT_TEST( testCreateRawDoc );
    CPPUNIT_TEST( testCreateVersionedDoc );
    CPPUNIT_TEST( testCreateMultipleRevisions );
    CPPUNIT_TEST( testAllDocs );
    CPPUNIT_TEST( testChanges );
    CPPUNIT_TEST( testRekey );
    CPPUNIT_TEST_SUITE_END();
};

C4EncryptionKey C4EncryptedDatabaseTest::sKey;

CPPUNIT_TEST_SUITE_REGISTRATION(C4EncryptedDatabaseTest);
