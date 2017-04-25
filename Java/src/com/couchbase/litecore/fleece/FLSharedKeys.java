package com.couchbase.litecore.fleece;

public class FLSharedKeys {
    private long handle = 0; // hold pointer to FLSharedKeys

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLSharedKeys(long handle) {
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // package level access
    //-------------------------------------------------------------------------
    long getHandle() {
        return handle;
    }
}
