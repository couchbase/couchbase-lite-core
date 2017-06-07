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

import com.couchbase.lite.Log;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public class C4Replicator {
    private static final String TAG = C4Replicator.class.getSimpleName();

    //-------------------------------------------------------------------------
    // Constant Variables
    //-------------------------------------------------------------------------

    public static final String kC4Replicator2Scheme = "blip";
    public static final String kC4Replicator2TLSScheme = "blips";

    //-------------------------------------------------------------------------
    // Static Variables
    //-------------------------------------------------------------------------

    // Long: handle of C4Replicator native address
    // C4Replicator: Java class holds handle
    private static Map<Long, C4Replicator> reverseLookupTable
            = Collections.synchronizedMap(new HashMap<Long, C4Replicator>());

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Replicator
    private C4ReplicatorListener listener = null;
    private Object context = null;

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    public C4Replicator(Database db,
                        String schema, String host, int port, String path,
                        String remoteDatabaseName,
                        Database otherLocalDB,
                        int push, int pull,
                        byte[] options,
                        C4ReplicatorListener listener, Object context) throws LiteCoreException {
        this.listener = listener;
        this.context = context;
        handle = create(db._handle, schema, host, port, path, remoteDatabaseName,
                otherLocalDB != null ? otherLocalDB._handle : 0,
                push, pull,
                options);
        reverseLookupTable.put(handle, this);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public void free() {
        if (handle != 0L)
            free(handle);
        handle = 0L;
    }

    public void stop() {
        if (handle != 0L)
            stop(handle);
    }

    public C4ReplicatorStatus getStatus() {
        if (handle != 0L)
            return getStatus(handle);
        else
            return null;
    }

    public byte[] getResponseHeaders() {
        if (handle != 0L)
            return getResponseHeaders(handle);
        else
            return null;
    }

    //-------------------------------------------------------------------------
    // public static methods
    //-------------------------------------------------------------------------
    public static boolean mayBeTransient(C4Error err) {
        return mayBeTransient(err.getDomain(), err.getCode(), err.getInternalInfo());
    }

    public static boolean mayBeNetworkDependent(C4Error err) {
        return mayBeNetworkDependent(err.getDomain(), err.getCode(), err.getInternalInfo());
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // callback methods from JNI
    //-------------------------------------------------------------------------

    private static void statusChangedCallback(long handle, C4ReplicatorStatus status) {
        Log.e(TAG, "statusChangedCallback() handle -> " + handle + ", status -> " + status);

        C4Replicator repl = reverseLookupTable.get(handle);
        if (repl != null && repl.listener != null)
            repl.listener.callback(repl, status, repl.context);
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    /**
     * Creates a new replicator.
     */
    private native static long create(long db,
                                      String schema, String host, int port, String path,
                                      String remoteDatabaseName,
                                      long otherLocalDB,
                                      int push, int pull,
                                      byte[] options) throws LiteCoreException;

    /**
     * Frees a replicator reference. If the replicator is running it will stop.
     */
    private native static void free(long replicator);

    /**
     * Tells a replicator to stop.
     */
    private native static void stop(long replicator);

    /**
     * Returns the current state of a replicator.
     */
    private native static C4ReplicatorStatus getStatus(long replicator);

    /**
     * Returns the HTTP response headers as a Fleece-encoded dictionary.
     */
    private native static byte[] getResponseHeaders(long replicator);

    /**
     * Returns true if this is a network error that may be transient,
     * i.e. the client should retry after a delay.
     */
    private native static boolean mayBeTransient(int domain, int code, int info);

    /**
     * Returns true if this error might go away when the network environment changes,
     * i.e. the client should retry after notification of a network status change.
     */
    private native static boolean mayBeNetworkDependent(int domain, int code, int info);
}
