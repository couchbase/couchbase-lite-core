/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */
package com.couchbase.litecore;

import android.content.Context;
import android.support.test.InstrumentationRegistry;
import android.util.Log;

import com.couchbase.litecore.fleece.FLSliceResult;
import com.couchbase.litecore.fleece.FLValue;

import org.junit.After;
import org.junit.Before;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import static com.couchbase.litecore.Constants.C4DocumentVersioning.kC4RevisionTrees;
import static com.couchbase.litecore.Constants.C4DocumentVersioning.kC4VersionVectors;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

/**
 * Ported from c4Test.cc
 */
public class BaseTest implements Constants {
    static {
        try {
            System.loadLibrary("sqlite3");
        } catch (Exception e) {
            fail("ERROR: Failed to load libsqlite3.so");
        }
        try {
            System.loadLibrary("LiteCoreJNI");
        } catch (Exception e) {
            fail("ERROR: Failed to load libLiteCoreJNI.so");
        }
    }

    public static final String LOG_TAG = BaseTest.class.getSimpleName();

    protected int encryptionAlgorithm() {
        return Database.NoEncryption;
    }

    protected byte[] encryptionKey() {
        return null;
    }

    protected Context context;
    protected File dir;
    protected Database db = null;
    protected int versioning = kC4RevisionTrees;

    protected static final String kDocID = "mydoc";
    protected static final String kRevID = "1-abcd";
    protected static final String kRev2ID = "2-c001d00d";
    protected static final String kRev3ID = "3-deadbeef";
    protected static final String kBody = "{\"name\":007}";

    protected boolean isRevTrees() {
        return versioning == kC4RevisionTrees;
    }

    protected boolean isVersionVectors() {
        return versioning == kC4VersionVectors;
    }

    public int getVersioning() {
        return versioning;
    }

    @Before
    public void setUp() throws Exception {
        context = InstrumentationRegistry.getContext();

        String dbFilename = "cbl_core_test.sqlite3";
        deleteDatabaseFile(dbFilename);
        context = InstrumentationRegistry.getContext();
        dir = new File(context.getFilesDir(), dbFilename);
        db = new Database(dir.getPath(), Database.Create | Database.SharedKeys, encryptionAlgorithm(), encryptionKey());
    }

    protected void deleteDatabaseFile(String dbFileName) {
        deleteFile(dbFileName);
    }

    private void deleteFile(String filename) {
        File file = new File(context.getFilesDir(), filename);
        if (file.exists()) {
            if (!file.delete()) {
                //Log.e(LOG_TAG, "ERROR failed to delete: dbFile=" + file);
            }
        }
    }

    @After
    public void tearDown() throws Exception {
        if (db != null) {
            db.close();
            db = null;
        }
    }

    protected void createRev(String docID, String revID, byte[] body) throws LiteCoreException {
        createRev(docID, revID, body, 0);
    }

    protected void createRev(Database db, String docID, String revID, byte[] body) throws LiteCoreException {
        createRev(db, docID, revID, body, 0);
    }

    protected void createRev(String docID, String revID, byte[] body, int flags) throws LiteCoreException {
        createRev(this.db, docID, revID, body, flags);
    }

    /**
     * @param flags C4RevisionFlags
     */
    protected void createRev(Database db, String docID, String revID, byte[] body, int flags) throws LiteCoreException {
        boolean commit = false;
        db.beginTransaction();
        try {
            Document doc = db.getDocument(docID, false);
            assertNotNull(doc);
            List<String> revIDs = new ArrayList<String>();
            revIDs.add(revID);
            if (doc.getRevID() != null)
                revIDs.add(doc.getRevID());
            String[] history = revIDs.toArray(new String[revIDs.size()]);
            db.put(docID, body, null, true, false, history, flags, true, 0);
            doc.free();
            commit = true;
        } finally {
            db.endTransaction(commit);
        }
    }

    protected long importJSONLines(File path) throws LiteCoreException, IOException {
        return importJSONLines(path, 15.0, true);
    }

    // Read a file that contains a JSON document per line. Every line becomes a document.
    protected long importJSONLines(File path, double timeout, boolean verbose) throws LiteCoreException, IOException {
        Log.i(LOG_TAG, String.format("Reading %s ...  ", path));
        StopWatch st = new StopWatch();
        long numDocs = 0;
        boolean commit = false;
        db.beginTransaction();
        try {
            BufferedReader br = new BufferedReader(new FileReader(path));
            try {
                String line = null;
                while ((line = br.readLine()) != null) {
                    FLSliceResult body = db.encodeJSON(line.getBytes());
                    String docID = String.format(Locale.ENGLISH, "%07d", numDocs + 1);
                    String[] history = new String[0];
                    Document doc = db.put(docID, body, null, false, false, history, 0, true, 0);
                    assertNotNull(doc);
                    doc.free();
                    body.free();
                    numDocs++;
                    if (numDocs % 1000 == 0 && st.getElapsedTimeSecs() >= timeout) {
                        String msg = String.format(Locale.ENGLISH, "Stopping JSON import after %.3f sec", st.getElapsedTimeSecs());
                        Log.w(LOG_TAG, msg);
                        throw new IOException(msg);
                    }
                    if (verbose && numDocs % 100000 == 0)
                        Log.i(LOG_TAG, String.valueOf(numDocs));
                }
            } finally {
                br.close();
            }
            commit = true;
        } finally {
            Log.i(LOG_TAG, "Committing...");
            db.endTransaction(commit);
        }

        if (verbose) Log.i(LOG_TAG, st.toString("Importing", numDocs, "doc"));
        return numDocs;
    }

    protected String json5(String input) {
        String json = null;
        try {
            json = FLValue.json5ToJson(input);
        } catch (LiteCoreException e) {
            Log.e(LOG_TAG, String.format("Error in json5() input -> %s", input), e);
            fail(e.getMessage());
        }
        assertNotNull(json);
        return json;
    }
}
