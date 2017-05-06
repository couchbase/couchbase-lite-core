package com.couchbase.litecore;

import android.support.test.InstrumentationRegistry;
import android.util.Log;

import com.couchbase.litecore.utils.Config;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.IOException;
import java.net.MalformedURLException;

import static com.couchbase.lite.utils.Config.TEST_PROPERTIES_FILE;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2Scheme;
import static com.couchbase.litecore.C4ReplicatorMode.kC4Disabled;
import static com.couchbase.litecore.C4ReplicatorMode.kC4OneShot;
import static com.couchbase.litecore.C4ReplicatorStatus.C4ReplicatorActivityLevel.kC4Stopped;

public class C4ReplicatorTest extends BaseTest {
    static final String TAG = C4ReplicatorTest.class.getSimpleName();

    com.couchbase.litecore.Database db2 = null;
    String schema = kC4Replicator2Scheme;
    C4Replicator repl = null;
    String host = "10.0.2.2";
    int port = 4984;
    String path = null;
    String remoteDB = "db";

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    @Before
    public void setUp() throws Exception {
        config = new Config(InstrumentationRegistry.getContext().getAssets().open(TEST_PROPERTIES_FILE));
        if(!config.replicatorTestsEnabled())
            return;

        super.setUp();

        this.host = config.remoteHost();
        this.port = config.remotePort();
        this.path = null;
        this.remoteDB = config.remoteDB();

        C4Socket.registerFactory();
    }

    @After
    public void tearDown() throws Exception {
        if(!config.replicatorTestsEnabled())
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

    // -- API Connection Failure
    @Test
    public void testAPIConnectionFailure() throws MalformedURLException, LiteCoreException {
        if(!config.replicatorTestsEnabled())
            return;

        this.host = "localhost";
        this.port = 1;
        replicate(kC4OneShot, kC4Disabled, false);
    }

    // -- API Push Empty DB
    @Test
    public void testAPIPushEmptyDB() throws MalformedURLException, LiteCoreException {
        if(!config.replicatorTestsEnabled())
            return;

        replicate(kC4OneShot, kC4Disabled, true);
    }

    // -- API Push Non-Empty DB
    @Test
    public void testAPIPushNonEmptyDB() throws IOException, LiteCoreException {
        if(!config.replicatorTestsEnabled())
            return;

        importJSONLines("names_100.json");
        replicate(kC4OneShot, kC4Disabled, true);
    }
    //-------------------------------------------------------------------------
    // internal methods
    //-------------------------------------------------------------------------
    void replicate(int push, int pull, boolean expectSuccess) throws LiteCoreException {
        repl = new C4Replicator(db, schema, host, port, path, remoteDB, db2, push, pull,
                new C4ReplicatorListener() {
                    @Override
                    public void callback(C4Replicator replicator, C4ReplicatorStatus status, Object context) {
                        Log.e(TAG, "C4ReplicatorListener.callback() repl -> " + repl + ", status -> " + status + ", context -> " + context);
                    }
                }, this);

        while (true) {
            C4ReplicatorStatus status = repl.getStatus();
            if (status.getActivityLevel() == kC4Stopped)
                break;
            try {
                Thread.sleep(300); // 300ms
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }

    }

}
