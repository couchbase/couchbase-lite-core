package com.couchbase.cbforest;

public class QueryIterator {

    QueryIterator(long handle) {
        _handle = handle;
    }

    public native boolean next() throws ForestException;

    public native byte[] keyJSON();
    public native byte[] valueJSON();
    public native String docID();

    public native void free();

    protected void finalize() {
        free();
    }

    private long _handle;  // Handle to native C4QueryEnumerator*
}
