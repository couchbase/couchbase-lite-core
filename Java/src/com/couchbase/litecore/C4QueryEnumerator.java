package com.couchbase.litecore;

public class C4QueryEnumerator {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------

    private long handle = 0; // hold pointer to C4QueryEnumerator

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    /* package */C4QueryEnumerator(long handle) {
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public byte[] getCustomColumns() {
        if (handle != 0)
            return getCustomColumns(handle);
        else
            return null;
    }

    public byte[] getFullTextMatched() throws LiteCoreException {
        if (handle != 0)
            return getFullTextMatched(handle);
        else
            return null;
    }

    public boolean next() throws LiteCoreException {
        boolean ok = next(handle);
        if (!ok)
            handle = 0;
        return ok;
    }

    public void close() {
        if (handle != 0)
            close(handle);
    }

    public void free() {
        if (handle != 0) {
            free(handle);
            handle = 0;
        }
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

    public long getFullTextTermCount() {
        return getFullTextTermCount(handle);
    }

    public long getFullTextTermIndex(long pos) {
        return getFullTextTermIndex(handle, pos);
    }

    public long getFullTextTermStart(long pos) {
        return getFullTextTermStart(handle, pos);
    }

    public long getFullTextTermLength(long pos) {
        return getFullTextTermLength(handle, pos);
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
    private static native byte[] getCustomColumns(long c4queryenumerator);

    /**
     * @param c4queryenumerator C4QueryEnumerator*
     * @return String (C4StringResult)
     * @throws LiteCoreException
     */
    private static native byte[] getFullTextMatched(long c4queryenumerator) throws LiteCoreException;

    private static native boolean next(long c4queryenumerator) throws LiteCoreException;

    private static native void close(long c4queryenumerator);

    private static native void free(long c4queryenumerator);

    // -- Accessor methods to C4QueryEnumerator --

    private static native String getDocID(long c4queryenumerator);

    private static native long getDocSequence(long c4queryenumerator);

    private static native String getRevID(long c4queryenumerator);

    private static native long getDocFlags(long c4queryenumerator);

    private static native long getFullTextTermCount(long c4queryenumerator);

    private static native long getFullTextTermIndex(long c4queryenumerator, long pos);  // C4FullTextTerm.termIndex

    private static native long getFullTextTermStart(long c4queryenumerator, long pos);  // C4FullTextTerm.start

    private static native long getFullTextTermLength(long c4queryenumerator, long pos); // C4FullTextTerm.length
}
