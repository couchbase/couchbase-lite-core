package com.couchbase.litecore.fleece;


public class FLSliceResult {
    private long handle = 0; // hold pointer to FLSliceResult

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLSliceResult(long handle) {
        this.handle = handle;
    }

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    public long getHandle() {
        return handle;
    }

    public void setHandle(long handle) {
        this.handle = handle;
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
     * Free FLArrayIterator instance
     *
     * @param slice (FLSliceResult)
     */
    private static native void free(long slice);
}
