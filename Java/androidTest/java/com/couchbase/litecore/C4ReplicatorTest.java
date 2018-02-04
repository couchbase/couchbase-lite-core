//
// C4ReplicatorTest.java
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
package com.couchbase.litecore;

import android.content.Context;
import android.support.test.InstrumentationRegistry;
import android.util.Log;

import com.couchbase.litecore.fleece.FLEncoder;
import com.couchbase.litecore.fleece.FLValue;
import com.couchbase.litecore.utils.FileUtils;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.net.MalformedURLException;
import java.util.Map;

import static com.couchbase.litecore.C4Constants.C4ErrorDomain.NetworkDomain;
import static com.couchbase.litecore.C4Constants.C4ErrorDomain.POSIXDomain;
import static com.couchbase.litecore.C4Constants.C4ErrorDomain.WebSocketDomain;
import static com.couchbase.litecore.C4Constants.NetworkError.kC4NetErrUnknownHost;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2Scheme;
import static com.couchbase.litecore.C4ReplicatorMode.kC4Continuous;
import static com.couchbase.litecore.C4ReplicatorMode.kC4Disabled;
import static com.couchbase.litecore.C4ReplicatorMode.kC4OneShot;
import static com.couchbase.litecore.C4ReplicatorStatus.C4ReplicatorActivityLevel.kC4Busy;
import static com.couchbase.litecore.C4ReplicatorStatus.C4ReplicatorActivityLevel.kC4Stopped;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * Ported from LiteCore/Replicator/tests/ReplicatorAPITest.cc
 */
public class C4ReplicatorTest extends C4BaseTest {
    static final String kScratchDBName = "scratch";
    static final String kITunesDBName = "itunes";
    static final String kWikipedia1kDBName = "wikipedia1k";
    static final String kProtectedDBName = "seekrit";

    com.couchbase.litecore.C4Database db2 = null;

    String schema = kC4Replicator2Scheme; // kC4Replicator2Scheme;
    C4Replicator repl = null;
    String host = "localhost";
    int port = 4984;
    String path = null;
    String remoteDB = null;
    byte[] options = null;

    C4ReplicatorStatus callbackStatus;
    int numCallbacks;
    int[] numCallbacksWithLevel;
    Map<String, Object> headers = null;

    //-------------------------------------------------------------------------
    // internal methods
    //-------------------------------------------------------------------------
    private void replicate(int push, int pull, boolean expectSuccess) throws LiteCoreException {
        repl = db.createReplicator(schema, host, port, path, remoteDB, db2, push, pull, options,
                new C4ReplicatorListener() {
                    @Override
                    public void statusChanged(C4Replicator replicator, C4ReplicatorStatus status, Object context) {
                        C4ReplicatorTest.this.callbackStatus = status;
                        C4ReplicatorTest.this.numCallbacks++;
                        numCallbacksWithLevel[status.getActivityLevel()] += 1;
                        Log.e(TAG, "C4ReplicatorListener.statusChanged() repl -> " + repl + ", status -> " + status + ", context -> " + context);

                        if (headers == null) {
                            byte[] h = replicator.getResponseHeaders();
                            if (h != null) {
                                headers = FLValue.fromData(h).asDict();
                                for (Map.Entry<String, Object> entry : headers.entrySet()) {
                                    Log.i(TAG, "Header: " + entry.getKey() + ": " + entry.getValue());
                                }
                            }
                        }
                    }

                    @Override
                    public void documentError(C4Replicator replicator, boolean pushing, String docID, C4Error error, boolean tran, Object context) {
                        // TODO:
                    }
                }, this);

        C4ReplicatorStatus status;
        while (true) {
            status = repl.getStatus();
            if (status.getActivityLevel() == kC4Stopped)
                break;
            try {
                Thread.sleep(300); // 300ms
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }


        assertTrue(numCallbacks > 0);
        if (expectSuccess) {
            assertEquals(0, status.getErrorCode());
            assertTrue(numCallbacksWithLevel[kC4Busy] > 0);
        }

        assertTrue(numCallbacksWithLevel[kC4Stopped] > 0);
        assertEquals(status.getActivityLevel(), callbackStatus.getActivityLevel());
        assertEquals(status.getErrorDomain(), callbackStatus.getErrorDomain());
        assertEquals(status.getErrorCode(), callbackStatus.getErrorCode());
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    @Before
    public void setUp() throws Exception {
        super.setUp();
        if (!config.replicatorTestsEnabled())
            return;

        this.host = config.remoteHost();
        this.port = config.remotePort();
        this.remoteDB = config.remoteDB();

        // Note: use CivetWeb instead of OkHttp for LiteCore level unit test
        // C4Socket.registerFactory();

        this.options = null;

        callbackStatus = null;
        numCallbacks = 0;
        numCallbacksWithLevel = new int[5];
        headers = null;
    }

    @After
    public void tearDown() throws Exception {
        if (!config.replicatorTestsEnabled())
            return;

        if (repl != null) {
            repl.stop();
            repl.free();
            repl = null;
        }
        if (db2 != null) {
            db2.free();
            db2 = null;
        }
        super.tearDown();
    }

    //-------------------------------------------------------------------------
    // tests
    //-------------------------------------------------------------------------

    // -- URL Parsing
    @Test
    public void testURLParsing() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        //TODO: need to implement
    }

    // -- API Connection Failure
    //    Try to connect to a nonexistent server (port 1 of localhost) and verify connection error.
    @Test
    public void testAPIConnectionFailure() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        this.host = "localhost";
        this.port = 1;
        replicate(kC4OneShot, kC4Disabled, false);
        assertEquals(0, callbackStatus.getProgressUnitsCompleted());
        assertEquals(0, callbackStatus.getProgressUnitsTotal());
        assertEquals(POSIXDomain, callbackStatus.getErrorDomain());
        assertEquals(111, callbackStatus.getErrorCode()); // ECONNREFUSED 111 -  Connection refused
    }

    // TODO: https://github.com/couchbase/couchbase-lite-core/issues/149
    // -- API DNS Lookup Failure
    //@Test
    public void testAPIDNSLookupFailure() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        this.host = "qux.ftaghn.miskatonic.edu";
        replicate(kC4OneShot, kC4Disabled, false);
        assertEquals(0, callbackStatus.getProgressUnitsCompleted());
        assertEquals(0, callbackStatus.getProgressUnitsTotal());
        assertEquals(NetworkDomain, callbackStatus.getErrorDomain());
        assertEquals(kC4NetErrUnknownHost, callbackStatus.getErrorCode());
    }

    // -- API Loopback Push
    @Test
    public void testAPILoopbackPush() throws LiteCoreException, IOException {
        if (!config.replicatorTestsEnabled())
            return;

        importJSONLines("names_100.json");

        String dbFilename2 = "cbl_core_test2.sqlite3";
        deleteDatabaseFile(dbFilename2);
        Context context2 = InstrumentationRegistry.getTargetContext();
        File dir2 = new File(context2.getFilesDir(), dbFilename2);
        FileUtils.cleanDirectory(dir2);
        db2 = new C4Database(dir2.getPath(), C4DatabaseFlags.kC4DB_Create | C4DatabaseFlags.kC4DB_SharedKeys, null, getVersioning(), encryptionAlgorithm(), encryptionKey());
        try {
            this.remoteDB = null;
            replicate(kC4OneShot, kC4Disabled, true);
            assertEquals(100, db2.getDocumentCount());
        } finally {
            if (db2 != null) {
                db2.close();
                db2.delete();
                db2.free();
                db2 = null;
            }
            FileUtils.cleanDirectory(dir2);
        }
    }

    // -- API Auth Failure
    @Test
    public void testAPIAuthFailure() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        this.remoteDB = kProtectedDBName;
        replicate(kC4OneShot, kC4Disabled, false);
        assertEquals(WebSocketDomain, callbackStatus.getErrorDomain());
        assertEquals(401, callbackStatus.getErrorCode());
        assertEquals("Basic realm=\"Couchbase Sync Gateway\"", headers.get("Www-Authenticate"));
    }

    // -- API ExtraHeaders
    @Test
    public void testAPIExtraHeaders() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        this.remoteDB = kProtectedDBName;

        // Use the extra-headers option to add HTTP Basic auth:
        FLEncoder enc = new FLEncoder();
        try {
            enc.beginDict(0);
            enc.writeKey("headers");
            enc.beginDict(0);
            enc.writeKey("Authorization");
            enc.writeString("Basic cHVwc2hhdzpmcmFuaw=="); // that's user 'pupshaw', password 'frank'
            enc.endDict();
            enc.endDict();
            this.options = enc.finish();
        } finally {
            enc.free();
        }
        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Push Empty DB
    @Test
    public void testAPIPushEmptyDB() throws MalformedURLException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Push Non-Empty DB
    @Test
    public void testAPIPushNonEmptyDB() throws IOException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        importJSONLines("names_100.json");
        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Push Big DB
    @Test
    public void testAPIPushBigDB() throws IOException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        importJSONLines("iTunesMusicLibrary.json");
        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Push Large-Docs DB
    //    Download https://github.com/diegoceccarelli/json-wikipedia/blob/master/src/test/resources/misc/en-wikipedia-articles-1000-1.json.gz
    //    and unzip to C/tests/data/ before running this test.
    @Test
    public void testAPIPushLargeDocsDB() throws IOException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        importJSONLines("en-wikipedia-articles-1000-1.json");
        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Pull
    @Test
    public void testAPIPull() throws IOException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        //this.remoteDB = kITunesDBName;
        replicate(kC4Disabled, kC4OneShot, true);
    }

    // -- API Continuous Pull
    @Test
    public void testAPIContinuousPull() throws IOException, LiteCoreException {
        if (!config.replicatorTestsEnabled())
            return;

        //this.remoteDB = kITunesDBName;
        replicate(kC4Disabled, kC4Continuous, true);
    }
}
