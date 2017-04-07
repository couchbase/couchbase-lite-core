package com.couchbase.litecore;

/**
 * Blob Key
 *
 * A raw SHA-1 digest used as the unique identifier of a blob.
 */
public class C4BlobKey {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4BlobKey

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    /**
     * Decodes a string of the form "sha1-"+base64 into a raw key.
     */
    public C4BlobKey(String str) throws LiteCoreException {
        handle = fromString(str);
    }

    /**
     * Encodes a blob key to a string of the form "sha1-"+base64.
     */
    public String toString() {
        return toString(handle);
    }

    public void free() {
        if (handle != 0L) {
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
    // package access
    //-------------------------------------------------------------------------
    C4BlobKey(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this.handle = handle;
    }

    long getHandle() {
        return handle;
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------
    private native static long fromString(String str) throws LiteCoreException;

    private native static String toString(long blobKey);

    private native static void free(long blobKey);
}
