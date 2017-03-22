package com.couchbase.litecore;

import android.util.Log;

import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.util.Locale;

import static com.couchbase.litecore.Constants.C4DocumentFlags.kConflicted;
import static com.couchbase.litecore.Constants.C4DocumentFlags.kExists;
import static com.couchbase.litecore.Constants.C4RevisionFlags.kRevKeepBody;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * Ported from c4DocumentTest.cc
 */
public class DocumentTest extends BaseTest {
    static final String LOG_TAG = "DocumentTest";

    @Override
    public void setUp() throws Exception {
        super.setUp();
        Utils.copyAssets("names_100.json", context.getFilesDir());
    }

    // - "FleeceDocs"
    @Test
    public void testFleeceDocs() throws LiteCoreException, IOException {
        importJSONLines(new File(context.getFilesDir(), "names_100.json"));
    }

    // - "Document PossibleAncestors"
    @Test
    public void testPossibleAncestors() throws LiteCoreException {
        if (!isRevTrees()) return;

        createRev(kDocID, kRevID, kBody.getBytes());
        createRev(kDocID, kRev2ID, kBody.getBytes());
        createRev(kDocID, kRev3ID, kBody.getBytes());

        Document doc = db.getDocument(kDocID, true);
        assertNotNull(doc);

        String newRevID = "3-f00f00";
        assertTrue(doc.selectFirstPossibleAncestorOf(newRevID));
        assertEquals(kRev2ID, doc.getSelectedRevID());
        assertTrue(doc.selectNextPossibleAncestorOf(newRevID));
        assertEquals(kRevID, doc.getSelectedRevID());
        assertFalse(doc.selectNextPossibleAncestorOf(newRevID));

        newRevID = "2-f00f00";
        assertTrue(doc.selectFirstPossibleAncestorOf(newRevID));
        assertEquals(kRevID, doc.getSelectedRevID());
        assertFalse(doc.selectNextPossibleAncestorOf(newRevID));

        newRevID = "1-f00f00";
        assertFalse(doc.selectFirstPossibleAncestorOf(newRevID));

        doc.free();
    }

    // - "Document CreateVersionedDoc"
    @Test
    public void testCreateVersionedDoc() throws LiteCoreException {
        // Try reading doc with mustExist=true, which should fail:
        try {
            Document doc = db.getDocument(kDocID, true);
            doc.free();
            fail();
        } catch (LiteCoreException lce) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, lce.domain);
            assertEquals(LiteCoreError.kC4ErrorNotFound, lce.code);
        }

        // Now get the doc with mustExist=false, which returns an empty doc:
        Document doc = db.getDocument(kDocID, false);
        assertNotNull(doc);
        assertEquals(0, doc.getFlags());
        assertEquals(kDocID, doc.getDocID());
        assertNull(doc.getRevID());
        assertNull(doc.getSelectedRevID());
        doc.free();

        boolean commit = false;
        db.beginTransaction();
        try {
            doc = db.put(kDocID, kBody.getBytes(), null, true, false, new String[]{kRevID}, 0, true, 0);
            assertNotNull(doc);
            assertEquals(kRevID, doc.getRevID());
            assertEquals(kRevID, doc.getSelectedRevID());
            assertEquals(kBody, new String(doc.getSelectedBody()));
            doc.free();
            commit = true;
        } finally {
            db.endTransaction(commit);
        }

        // Reload the doc:
        doc = db.getDocument(kDocID, true);
        assertNotNull(doc);
        assertEquals(kExists, doc.getFlags());
        assertEquals(kDocID, doc.getDocID());
        assertEquals(kRevID, doc.getRevID());
        assertEquals(kRevID, doc.getSelectedRevID());
        assertEquals(1, doc.getSelectedSequence());
        assertEquals(kBody, new String(doc.getSelectedBody()));
        doc.free();

        // Get the doc by its sequence:
        doc = db.getDocumentBySequence(1);
        assertNotNull(doc);
        assertEquals(kExists, doc.getFlags());
        assertEquals(kDocID, doc.getDocID());
        assertEquals(kRevID, doc.getRevID());
        assertEquals(kRevID, doc.getSelectedRevID());
        assertEquals(1, doc.getSelectedSequence());
        assertEquals(kBody, new String(doc.getSelectedBody()));
        doc.free();
    }

    // - "Document CreateMultipleRevisions"
    @Test
    public void testCreateMultipleRevisions() throws LiteCoreException {
        final String kBody2 = "{\"ok\":go}";
        final String kBody3 = "{\"ubu\":roi}";
        createRev(kDocID, kRevID, kBody.getBytes());
        createRev(kDocID, kRev2ID, kBody2.getBytes(), kRevKeepBody);
        createRev(kDocID, kRev2ID, kBody2.getBytes()); // test redundant insert

        // Reload the doc:
        Document doc = db.getDocument(kDocID, true);
        assertNotNull(doc);
        assertEquals(kExists, doc.getFlags());
        assertEquals(kDocID, doc.getDocID());
        assertEquals(kRev2ID, doc.getRevID());
        assertEquals(kRev2ID, doc.getSelectedRevID());
        assertEquals(2, doc.getSelectedSequence());
        assertEquals(kBody2, new String(doc.getSelectedBody()));

        if (isRevTrees()) {
            // Select 1st revision:
            assertTrue(doc.selectParentRev());
            assertEquals(kRevID, doc.getSelectedRevID());
            assertEquals(1, doc.getSelectedSequence());
            try {
                doc.getSelectedBody();
                fail();
            } catch (LiteCoreException e) {
            }
            assertFalse(doc.hasRevisionBody());
            assertFalse(doc.selectParentRev());
            doc.free();

            // Add a 3rd revision:
            createRev(kDocID, kRev3ID, kBody3.getBytes());

            // Revision 2 should keep its body due to the kRevKeepBody flag:
            doc = db.getDocument(kDocID, true);
            assertNotNull(doc);
            assertTrue(doc.selectParentRev());
            assertEquals(kDocID, doc.getDocID());
            assertEquals(kRev3ID, doc.getRevID());
            assertEquals(kRev2ID, doc.getSelectedRevID());
            assertEquals(2, doc.getSelectedSequence());
            assertEquals(kBody2, new String(doc.getSelectedBody()));
            assertTrue(doc.getSelectedRevFlags() == kRevKeepBody);
            doc.free();

            // Purge doc
            boolean commit = false;
            db.beginTransaction();
            try {
                doc = db.getDocument(kDocID, true);
                int nPurged = doc.purgeRevision(kRev3ID);
                assertEquals(3, nPurged);
                doc.save(20);
                commit = true;
            } finally {
                db.endTransaction(commit);
            }
        }
        doc.free();
    }

    // - "Document maxRevTreeDepth"
    @Test
    public void testMaxRevTreeDepth() throws LiteCoreException {
        if (isRevTrees()) {
            // NOTE: c4db_getMaxRevTreeDepth and c4db_setMaxRevTreeDepth are not supported by JNI.
        }
        final int kNumRevs = 10000;
        StopWatch st = new StopWatch();
        Document doc = db.getDocument(kDocID, false);
        assertNotNull(doc);
        boolean commit = false;
        db.beginTransaction();
        try {
            for (int i = 0; i < kNumRevs; i++) {
                String[] history = {doc.getRevID()};
                Document savedDoc = db.put(doc.getDocID(), kBody.getBytes(), null, false, false, history, 0, true, 30);
                assertNotNull(savedDoc);
                doc.free();
                doc = savedDoc;
            }
            commit = true;
        } finally {
            db.endTransaction(commit);
        }
        Log.i(LOG_TAG, String.format(Locale.ENGLISH, "Created %d revisions in %.3f ms", kNumRevs, st.getElapsedTimeMillis()));

        // Check rev tree depth:
        int nRevs = 0;
        assertTrue(doc.selectCurrentRev());
        do {
            if (isRevTrees())
                // NOTE: c4rev_getGeneration is not supported.
                ;
            nRevs++;
        } while (doc.selectParentRev());
        Log.i(LOG_TAG, String.format(Locale.ENGLISH, "Document rev tree depth is %d", nRevs));
        if (isRevTrees())
            assertEquals(30, nRevs);
        doc.free();
    }

    // - "Document GetForPut"
    @Test
    public void testGetForPut() throws LiteCoreException {
        // NOTE: c4doc_getForPut is not supported by JNI, and not used from Lite (Java)
    }

    // - "Document Put"
    @Test
    public void testPut() throws LiteCoreException {
        boolean commit = false;
        db.beginTransaction();
        try {
            // Creating doc given ID:
            Document doc = db.put(kDocID, kBody.getBytes(), null, false, false, new String[0], 0, true, 0);
            assertNotNull(doc);
            assertEquals(kDocID, doc.getDocID());
            String kExpectedRevID = isRevTrees() ?
                    "1-c10c25442d9fe14fa3ca0db4322d7f1e43140fab" :
                    "1@*";
            assertEquals(kExpectedRevID, doc.getRevID());
            assertEquals(kExists, doc.getFlags());
            assertEquals(kExpectedRevID, doc.getSelectedRevID());
            doc.free();

            // Update doc:
            String[] history = {kExpectedRevID};
            doc = db.put(kDocID, "{\"ok\":\"go\"}".getBytes(), null, false, false, history, 0, true, 0);
            assertNotNull(doc);
            // NOTE: With current JNI binding, unable to check commonAncestorIndex value
            String kExpectedRevID2 = isRevTrees() ?
                    "2-32c711b29ea3297e27f3c28c8b066a68e1bb3f7b" :
                    "2@*";
            assertEquals(kExpectedRevID2, doc.getRevID());
            assertEquals(kExists, doc.getFlags());
            assertEquals(kExpectedRevID2, doc.getSelectedRevID());
            doc.free();

            // Insert existing rev that conflicts:
            String kConflictRevID = isRevTrees() ?
                    "2-deadbeef" :
                    "1@binky";
            String[] history2 = {kConflictRevID, kExpectedRevID};
            doc = db.put(kDocID, "{\"from\":\"elsewhere\"}".getBytes(), null, true, true, history2, 0, true, 0);
            assertNotNull(doc);
            // NOTE: With current JNI binding, unable to check commonAncestorIndex value
            assertEquals(kConflictRevID, doc.getRevID());
            assertEquals(kExists | kConflicted, doc.getFlags());
            if (isRevTrees())
                assertEquals(kConflictRevID, doc.getSelectedRevID());
            else
                assertEquals(kExpectedRevID2, doc.getSelectedRevID());
            doc.free();

            commit = true;
        } finally {
            db.endTransaction(commit);
        }
    }
}
