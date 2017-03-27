package com.couchbase.litecore;

public class C4QueryEnumerator {
    private long handle; // hold pointer to C4QueryEnumerator

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    /* package */C4QueryEnumerator(long handle) {
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public String fullTextMatched() throws LiteCoreException {
        return fullTextMatched(handle);
    }

    public boolean next() throws LiteCoreException {
        return next(handle);
    }

    public void close() {
        close(handle);
    }

    public void free() {
        free(handle);
    }

    public String getDocID() {
        return getDocID(handle);
    }

    public long getDocSequence() {
        return getDocSequence(handle);
    }

    public String getRevID() {
        return getRevID(handle);
    }

    public long getDocFlags() {
        return getDocFlags(handle);
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
    // native methods
    //-------------------------------------------------------------------------


    /**
     * @param e C4QueryEnumerator*
     * @return C4SliceResult
     */
    //private static native long customColumns(long e);

    /**
     * @param e C4QueryEnumerator*
     * @return C4StringResult
     * @throws LiteCoreException
     */
    private static native String fullTextMatched(long e) throws LiteCoreException;


    private static native boolean next(long e) throws LiteCoreException;

    private static native void close(long e);

    private static native void free(long e);

    // Accessor method to C4QueryEnumerator
    private static native String getDocID(long e);

    private static native long getDocSequence(long e);

    private static native String getRevID(long e);

    private static native long getDocFlags(long e);
    //private static native long getFullTextTermCount(long e);
}
