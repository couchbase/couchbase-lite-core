//
// C4Replicator.java
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

import android.util.Log;

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

    C4Replicator(long db,
                 String schema, String host, int port, String path,
                 String remoteDatabaseName,
                 long otherLocalDB,
                 int push, int pull,
                 byte[] options,
                 C4ReplicatorListener listener, Object context) throws LiteCoreException {
        this.listener = listener;
        this.context = context;
        handle = create(db, schema, host, port, path, remoteDatabaseName,
                otherLocalDB,
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
        C4Replicator repl = reverseLookupTable.get(handle);
        if (repl != null && repl.listener != null)
            repl.listener.statusChanged(repl, status, repl.context);
    }

    private static void documentErrorCallback(long handle, boolean pushing,
                                              String docID, int domain, int code, int internalInfo,
                                              boolean trans) {
        Log.e(TAG, "documentErrorCallback() handle -> " + handle +
                ", pushing -> " + pushing +
                ", docID -> " + docID +
                ", domain -> " + domain +
                ", code -> " + code +
                ", internalInfo -> " + internalInfo +
                ", trans -> " + trans);

        C4Replicator repl = reverseLookupTable.get(handle);
        if (repl != null && repl.listener != null) {
            repl.listener.documentError(repl, pushing, docID,
                    new C4Error(domain, code, internalInfo), trans, repl.context);
        }
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    /**
     * Creates a new replicator.
     */
    static native long create(long db,
                              String schema, String host, int port, String path,
                              String remoteDatabaseName,
                              long otherLocalDB,
                              int push, int pull,
                              byte[] options) throws LiteCoreException;

    /**
     * Frees a replicator reference. If the replicator is running it will stop.
     */
    static native void free(long replicator);

    /**
     * Tells a replicator to stop.
     */
    static native void stop(long replicator);

    /**
     * Returns the current state of a replicator.
     */
    static native C4ReplicatorStatus getStatus(long replicator);

    /**
     * Returns the HTTP response headers as a Fleece-encoded dictionary.
     */
    static native byte[] getResponseHeaders(long replicator);

    /**
     * Returns true if this is a network error that may be transient,
     * i.e. the client should retry after a delay.
     */
    static native boolean mayBeTransient(int domain, int code, int info);

    /**
     * Returns true if this error might go away when the network environment changes,
     * i.e. the client should retry after notification of a network status change.
     */
    static native boolean mayBeNetworkDependent(int domain, int code, int info);
}
