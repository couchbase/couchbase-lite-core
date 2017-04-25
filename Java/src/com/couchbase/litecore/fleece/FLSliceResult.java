package com.couchbase.litecore.fleece;


public class FLSliceResult {
    private long handle = 0; // hold pointer to FLSliceResult

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLSliceResult(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
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

    public byte[] getBuf() {
        return getBuf(handle);
    }

    public long getSize() {
        return getSize(handle);
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

    private static native byte[] getBuf(long slice);

    private static native long getSize(long slice);
}
