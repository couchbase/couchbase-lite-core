package com.couchbase.litecore;


import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public class C4DocumentObserver {
    //-------------------------------------------------------------------------
    // Static Variables
    //-------------------------------------------------------------------------
    // Long: handle of C4DatabaseObserver native address
    // C4DocumentObserver: Java class holds handle
    private static Map<Long, C4DocumentObserver> reverseLookupTable
            = Collections.synchronizedMap(new HashMap<Long, C4DocumentObserver>());

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4DocumentObserver
    private C4DocumentObserverListener listener = null;
    private Object context = null;

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    public C4DocumentObserver(Database db, String docID, C4DocumentObserverListener listener, Object context) {
        this.listener = listener;
        this.context = context;
        handle = create(db._handle, docID);
        reverseLookupTable.put(handle, this);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public void free() {
        if (handle != 0L) {
            reverseLookupTable.remove(handle);
            free(handle);
            handle = 0L;
        }
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

    /**
     * Callback invoked by a database observer.
     * <p>
     * NOTE: Two parameters, observer and context, which are defined for iOS:
     * observer -> this instance
     * context ->  maintained in java layer
     */
    private static void callback(long handle, String docID, long sequence) {
        C4DocumentObserver obs = reverseLookupTable.get(handle);
        if (obs != null && obs.listener != null)
            obs.listener.callback(obs, docID, sequence, obs.context);
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    private native static long create(long db, String docID);

    /**
     * Free C4DocumentObserver* instance
     *
     * @param c4observer (C4DocumentObserver*)
     */
    private static native void free(long c4observer);
}
