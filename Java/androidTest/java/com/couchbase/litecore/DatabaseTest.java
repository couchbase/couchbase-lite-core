/**
 * Created by Hideki Itakura on 10/20/2015.
 * Copyright (c) 2015 Couchbase, Inc All rights reserved.
 * <p/>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p/>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p/>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */
package com.couchbase.litecore;

import org.junit.Test;

import java.io.File;
import java.util.Arrays;
import java.util.Locale;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * Ported from c4DatabaseTest.cc
 */
public class DatabaseTest extends BaseTest {

    // - "Database ErrorMessages"
    @Test
    public void testDatabaseErrorMessages() {
        try {
            new Database("", 0, 0, null);
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorCantOpenFile, e.code);
            assertEquals("unable to open database file", e.getMessage());
        }

        try {
            db.getDocument("a", true);
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorNotFound, e.code);
            assertEquals("not found", e.getMessage());
        }

        try {
            db.getDocument(null, true);
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorNotFound, e.code);
            assertEquals("not found", e.getMessage());
        }

        // NOTE: c4error_getMessage() is not supported by Java
    }

    // - "Database Info"
    @Test
    public void testDatabaseInfo() {
        assertEquals(0, db.getDocumentCount());
        assertEquals(0, db.getLastSequence());

        // NOTE: c4db_getUUIDs() is not supported by Java
    }


    // - "Database OpenBundle"
    @Test
    public void testDatabaseOpenBundle() throws LiteCoreException {
        File bundlePath = new File(context.getFilesDir(), "cbl_core_test_bundle");

        if (bundlePath.exists())
            Database.deleteAtPath(bundlePath.getPath(), Database.Create | Database.Bundle);
        Database bundle = new Database(bundlePath.getPath(), Database.Create | Database.Bundle, encryptionAlgorithm(), encryptionKey());
        assertNotNull(bundle);
        bundle.close();
        bundle.free();

        // Reopen without 'create' flag:
        bundle = new Database(bundlePath.getPath(), Database.Bundle, encryptionAlgorithm(), encryptionKey());
        assertNotNull(bundle);
        bundle.close();
        bundle.free();

        // Reopen with wrong storage type:
        // TODO: Need to support storage type some point?

        // Open nonexistent bundle:
        File noSuchBundlePath = new File(context.getFilesDir(), "no_such_bundle");
        try {
            bundle = new Database(noSuchBundlePath.getPath(), Database.Bundle, encryptionAlgorithm(), encryptionKey());
            fail();
        } catch (LiteCoreException e) {
        }
    }

    // - "Database Transaction"
    @Test
    public void testDatabaseTransaction() throws LiteCoreException {
        assertEquals(0, db.getDocumentCount());
        assertFalse(db.isInTransaction());
        db.beginTransaction();
        assertTrue(db.isInTransaction());
        db.beginTransaction();
        assertTrue(db.isInTransaction());
        db.endTransaction(true);
        assertTrue(db.isInTransaction());
        db.endTransaction(true);
        assertFalse(db.isInTransaction());
    }

    // - "Database CreateRawDoc"
    @Test
    public void testDatabaseCreateRawDoc() throws LiteCoreException {
        final String store = "test";
        final String key = "key";
        final String meta = "meta";
        boolean commit = false;
        db.beginTransaction();
        try {
            db.rawPut(store, key, meta.getBytes(), kBody.getBytes());
            commit = true;
        } finally {
            db.endTransaction(commit);
        }

        byte[][] metaNbody = db.rawGet(store, key);
        assertNotNull(metaNbody);
        assertEquals(2, metaNbody.length);
        assertTrue(Arrays.equals(meta.getBytes(), metaNbody[0]));
        assertTrue(Arrays.equals(kBody.getBytes(), metaNbody[1]));

        // Nonexistent:
        try {
            db.rawGet(store, "bogus");
            fail("Should not come here.");
        } catch (LiteCoreException ex) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, ex.domain);
            assertEquals(LiteCoreError.kC4ErrorNotFound, ex.code);
        }
    }

    // - "Database AllDocs"
    @Test
    public void testDatabaseAllDocs() throws LiteCoreException {
        setupAllDocs();

        assertEquals(99, db.getDocumentCount());

        // No start or end ID:
        int iteratorFlags = IteratorFlags.kDefault;
        iteratorFlags &= ~IteratorFlags.kIncludeBodies;
        DocumentIterator itr = db.iterator(null, null, 0, iteratorFlags);
        assertNotNull(itr);
        Document doc;
        int i = 1;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID = String.format(Locale.ENGLISH, "doc-%03d", i);
                assertEquals(docID, doc.getDocID());
                assertEquals(kRevID, doc.getRevID());
                assertEquals(kRevID, doc.getSelectedRevID());
                assertEquals(i, doc.getSelectedSequence());
                assertNull(doc.getSelectedBodyTest());
                // Doc was loaded without its body, but it should load on demand:
                assertTrue(Arrays.equals(kBody.getBytes(), doc.getSelectedBody()));
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(100, i);

        // Start and end ID:
        itr = db.iterator("doc-007", "doc-090", 0, IteratorFlags.kDefault);
        assertNotNull(itr);
        i = 7;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID = String.format(Locale.ENGLISH, "doc-%03d", i);
                assertEquals(docID, doc.getDocID());
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(91, i);

        // Some docs, by ID:
        String[] docIDs = {"doc-042", "doc-007", "bogus", "doc-001"};
        iteratorFlags = IteratorFlags.kDefault;
        iteratorFlags |= IteratorFlags.kIncludeDeleted;
        itr = db.iterator(docIDs, iteratorFlags);
        assertNotNull(itr);
        i = 0;
        while ((doc = itr.nextDocument()) != null) {
            try {
                assertEquals(docIDs[i], doc.getDocID());
                assertEquals(i != 2, doc.getSelectedSequence() != 0);
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(4, i);
    }

    // - "Database AllDocsIncludeDeleted"
    @Test
    public void testDatabaseAllDocsIncludeDeleted() throws LiteCoreException {
        setupAllDocs();

        int iteratorFlags = IteratorFlags.kDefault;
        iteratorFlags |= IteratorFlags.kIncludeDeleted;
        DocumentIterator itr = db.iterator("doc-004", "doc-007", 0, iteratorFlags);
        assertNotNull(itr);
        Document doc;
        int i = 4;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID;
                if (i == 6)
                    docID = "doc-005DEL";
                else
                    docID = String.format(Locale.ENGLISH, "doc-%03d", i >= 6 ? i - 1 : i);
                assertEquals(docID, doc.getDocID());
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(9, i);
    }

    // - "Database AllDocsInfo"
    @Test
    public void testAllDocsInfo() throws LiteCoreException {
        setupAllDocs();

        // No start or end ID:
        int iteratorFlags = IteratorFlags.kDefault;
        DocumentIterator itr = db.iterator(null, null, 0, iteratorFlags);
        assertNotNull(itr);
        Document doc;
        int i = 1;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID = String.format(Locale.ENGLISH, "doc-%03d", i);
                assertEquals(docID, doc.getDocID());
                assertEquals(kRevID, doc.getRevID());
                assertEquals(kRevID, doc.getSelectedRevID());
                assertEquals(i, doc.getSequence());
                assertEquals(i, doc.getSelectedSequence());
                assertEquals(C4DocumentFlags.kExists, doc.getFlags());
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(100, i);
    }

    // - "Database Changes"
    @Test
    public void testDatabaseChanges() throws LiteCoreException {
        for (int i = 1; i < 100; i++) {
            String docID = String.format(Locale.ENGLISH, "doc-%03d", i);
            createRev(docID, kRevID, kBody.getBytes());
        }

        // Since start:
        int iteratorFlags = IteratorFlags.kDefault;
        iteratorFlags &= ~IteratorFlags.kIncludeBodies;
        DocumentIterator itr = new DocumentIterator(db._handle, 0, iteratorFlags);
        assertNotNull(itr);
        Document doc;
        long seq = 1;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID = String.format(Locale.ENGLISH, "doc-%03d", seq);
                assertEquals(docID, doc.getDocID());
                assertEquals(seq, doc.getSelectedSequence());
                seq++;
            } finally {
                doc.free();
            }
        }
        assertEquals(100L, seq);

        // Since 6:
        itr = new DocumentIterator(db._handle, 6, iteratorFlags);
        assertNotNull(itr);
        seq = 7;
        while ((doc = itr.nextDocument()) != null) {
            try {
                String docID = String.format(Locale.ENGLISH, "doc-%03d", seq);
                assertEquals(docID, doc.getDocID());
                assertEquals(seq, doc.getSelectedSequence());
                seq++;
            } finally {
                doc.free();
            }
        }
        assertEquals(100L, seq);
    }

    // - "Database Expired"
    @Test
    public void testDatabaseExpired() throws LiteCoreException {
        String docID = "expire_me";
        createRev(docID, kRevID, kBody.getBytes());

        // unix time
        long expire = System.currentTimeMillis() / 1000 + 1;
        db.setExpiration(docID, expire);

        expire = System.currentTimeMillis() / 1000 + 2;
        db.setExpiration(docID, expire);
        db.setExpiration(docID, expire);

        String docID2 = "expire_me_too";
        createRev(docID2, kRevID, kBody.getBytes());
        db.setExpiration(docID2, expire);

        String docID3 = "dont_expire_me";
        createRev(docID3, kRevID, kBody.getBytes());
        try {
            Thread.sleep(2 * 1000); // sleep 2 sec
        } catch (InterruptedException e) {
        }

        assertEquals(expire, db.expirationOfDoc(docID));
        assertEquals(expire, db.expirationOfDoc(docID2));
        assertEquals(expire, db.nextDocExpiration());

        // TODO: DB00x - Java does not support c4db_enumerateExpired and c4exp_next
    }

    // - "Database CancelExpire"
    @Test
    public void testDatabaseCancelExpire() throws LiteCoreException {
        String docID = "expire_me";
        createRev(docID, kRevID, kBody.getBytes());

        // unix time
        long expire = System.currentTimeMillis() / 1000 + 2;
        db.setExpiration(docID, expire);
        db.setExpiration(docID, Long.MAX_VALUE);

        try {
            Thread.sleep(2 * 1000); // sleep 2 sec
        } catch (InterruptedException e) {
        }

        // TODO: DB00x - Java does not support c4db_enumerateExpired, c4exp_next and c4exp_purgeExpired with enumerator.
    }

    // - "Database BlobStore"
    @Test
    public void testDatabaseBlobStore() throws LiteCoreException {
        // TODO: DB005 - Java does not support c4db_getBlobStore.
    }

    private void setupAllDocs() throws LiteCoreException {
        for (int i = 1; i < 100; i++) {
            String docID = String.format(Locale.ENGLISH, "doc-%03d", i);
            createRev(docID, kRevID, kBody.getBytes());
        }

        // Add a deleted doc to make sure it's skipped by default:
        createRev("doc-005DEL", kRevID, null, C4RevisionFlags.kRevDeleted);
    }
}
