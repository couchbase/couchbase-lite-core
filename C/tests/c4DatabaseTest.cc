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
#ifdef _MSC_VER
#define random() rand()
#endif

class C4DatabaseTest : public C4Test {
    public:

    void testTransaction() {
        AssertEqual(c4db_getDocumentCount(db), (C4SequenceNumber)0);
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

            AssertEqual(c4doc_insertRevision(doc, kRevID, kBody, false, false, false, &error), 1);

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
        AssertEqual(doc->flags, (C4DocumentFlags)kExists);
        AssertEqual(doc->docID, kDocID);
        AssertEqual(doc->revID, kRevID);
        AssertEqual(doc->selectedRev.revID, kRevID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)1);
        AssertEqual(doc->selectedRev.body, kBody);

        // Get the doc by its sequence:
        doc = c4doc_getBySequence(db, 1, &error);
        Assert(doc != NULL);
        AssertEqual(doc->flags, (C4DocumentFlags)kExists);
        AssertEqual(doc->docID, kDocID);
        AssertEqual(doc->revID, kRevID);
        AssertEqual(doc->selectedRev.revID, kRevID);
        AssertEqual(doc->selectedRev.sequence, (C4SequenceNumber)1);
        AssertEqual(doc->selectedRev.body, kBody);
    }


    void testCreateMultipleRevisions() {
        const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
        createRev(kDocID, kRevID, kBody);
        createRev(kDocID, kRev2ID, kBody2);
        createRev(kDocID, kRev2ID, kBody2, false); // test redundant insert

        // Reload the doc:
        C4Error error;
        C4Document *doc = c4doc_get(db, kDocID, true, &error);
        Assert(doc != NULL);
        AssertEqual(doc->flags, (C4DocumentFlags)kExists);
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

        // Purge doc
        {
            TransactionHelper t(db);
            int nPurged = c4doc_purgeRevision(doc, kRev2ID, &error);
            AssertEqual(nPurged, 2);
            Assert(c4doc_save(doc, 20, &error));
        }
    }


    void testInsertRevisionWithHistory() {
        const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
        createRev(kDocID, kRevID, kBody);
        createRev(kDocID, kRev2ID, kBody2);

        // Reload the doc:
        C4Error error;
        C4Document *doc = c4doc_get(db, kDocID, true, &error);

        // Add 18 revisions; the last two entries in the history repeat the two existing revs:
        const unsigned kHistoryCount = 20;
        std::vector<std::string> revIDs;
        revIDs.reserve(kHistoryCount);
        for (unsigned i = kHistoryCount - 1; i >= 2; i--) {
            char buf[20];
            sprintf(buf, "%u-%08lx", i+1, (unsigned long)random());
            std::string str(buf);
            revIDs.push_back(str);
        }
        revIDs.push_back(toString(kRev2ID));
        revIDs.push_back(toString(kRevID));

        C4Slice history[kHistoryCount];
        for (unsigned i = 0; i < kHistoryCount; i++) {
            history[i] = c4str(revIDs[i].c_str());
        }

        int n;
        {
            TransactionHelper t(db);
            n = c4doc_insertRevisionWithHistory(doc, c4str("{\"foo\":true}"),
                                                false, false,
                                                history, kHistoryCount, &error);
        }
        if (n < 0)
            std::cerr << "Error(" << error.domain << "," << error.code << ")\n";
        AssertEqual(n, (int)(kHistoryCount-2));
    }


    void setupAllDocs() {
        char docID[20];
        for (int i = 1; i < 100; i++) {
            sprintf(docID, "doc-%03d", i);
            createRev(c4str(docID), kRevID, kBody);
        }
        // Add a deleted doc to make sure it's skipped by default:
        createRev(c4str("doc-005DEL"), kRevID, kC4SliceNull);
    }


    void testAllDocs() {
        setupAllDocs();
        C4Error error;
        C4DocEnumerator* e;
        C4Document* doc;

        // No start or end ID:
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
        Assert(e);
        char docID[20];
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
        c4enum_free(e);

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
        c4enum_free(e);
        AssertEqual(i, 91);

        // Some docs, by ID:
        options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        C4Slice docIDs[4] = {C4STR("doc-042"), C4STR("doc-007"), C4STR("bogus"), C4STR("doc-001")};
        e = c4db_enumerateSomeDocs(db, docIDs, 4, &options, &error);
        Assert(e);
        i = 0;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            AssertEqual(doc->docID, docIDs[i]);
            AssertEqual(doc->sequence != 0, i != 2);
            c4doc_free(doc);
            i++;
        }
        c4enum_free(e);
        AssertEqual(i, 4);
    }


    void testAllDocsIncludeDeleted() {
        char docID[20];
        setupAllDocs();

        C4Error error;
        C4DocEnumerator* e;
        C4Document* doc;

        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        e = c4db_enumerateAllDocs(db, c4str("doc-004"), c4str("doc-007"), &options, &error);
        Assert(e);
        int i = 4;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            if (i == 6)
                strcpy(docID, "doc-005DEL");
            else
                sprintf(docID, "doc-%03d", i - (i>=6));
            AssertEqual(doc->docID, c4str(docID));
            c4doc_free(doc);
            i++;
        }
        c4enum_free(e);
        AssertEqual(i, 9);
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
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
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
        c4enum_free(e);

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
        c4enum_free(e);
        AssertEqual(seq, (C4SequenceNumber)100);
    }


    CPPUNIT_TEST_SUITE( C4DatabaseTest );
    CPPUNIT_TEST( testTransaction );
    CPPUNIT_TEST( testCreateRawDoc );
    CPPUNIT_TEST( testCreateVersionedDoc );
    CPPUNIT_TEST( testCreateMultipleRevisions );
    CPPUNIT_TEST( testInsertRevisionWithHistory );
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
    CPPUNIT_TEST( testAllDocsIncludeDeleted );
    CPPUNIT_TEST( testChanges );
    CPPUNIT_TEST( testRekey );
    CPPUNIT_TEST_SUITE_END();
};

C4EncryptionKey C4EncryptedDatabaseTest::sKey;

CPPUNIT_TEST_SUITE_REGISTRATION(C4EncryptedDatabaseTest);
