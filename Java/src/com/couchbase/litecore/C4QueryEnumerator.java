package com.couchbase.litecore;

import com.couchbase.litecore.fleece.FLSliceResult;

public class C4QueryEnumerator {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------

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
    public FLSliceResult customColumns() {
        return new FLSliceResult(customColumns(handle));
    }

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

    // -- Accessor methods to C4QueryEnumerator --

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
     * @param c4queryenumerator C4QueryEnumerator*
     * @return C4SliceResult(FLSliceResult)
     */
    private static native long customColumns(long c4queryenumerator);

    /**
     * @param c4queryenumerator C4QueryEnumerator*
     * @return String (C4StringResult)
     * @throws LiteCoreException
     */
    private static native String fullTextMatched(long c4queryenumerator) throws LiteCoreException;

    private static native boolean next(long c4queryenumerator) throws LiteCoreException;

    private static native void close(long c4queryenumerator);

    private static native void free(long c4queryenumerator);

    // -- Accessor methods to C4QueryEnumerator --

    private static native String getDocID(long c4queryenumerator);

    private static native long getDocSequence(long c4queryenumerator);

    private static native String getRevID(long c4queryenumerator);

    private static native long getDocFlags(long c4queryenumerator);

    //private static native long getFullTextTermCount(long c4queryenumerator);
    //private static native long getFullTextTerms(long c4queryenumerator);
}
