package com.couchbase.litecore;

public class C4RawDocument {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Database

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    C4RawDocument(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public String key() {
        return key(handle);
    }

    public String meta() {
        return meta(handle);
    }

    public String body() {
        return body(handle);
    }

    public void free() throws LiteCoreException {
        if (handle != 0L) {
            C4Database.rawFree(handle);
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
    // native methods
    //-------------------------------------------------------------------------

    static native String key(long rawDoc);

    static native String meta(long rawDoc);

    static native String body(long rawDoc);
}
