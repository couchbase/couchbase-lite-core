package com.couchbase.litecore;

/**
 * An open stream for writing data to a blob.
 */
public class C4BlobWriteStream {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4BlobReadStream

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    /**
     * Writes data to a stream.
     */
    public void write(byte[] bytes) throws LiteCoreException {
        write(handle, bytes);
    }

    /**
     * Computes the blob-key (digest) of the data written to the stream. This should only be
     * called after writing the entire data. No more data can be written after this call.
     */
    public C4BlobKey computeBlobKey() throws LiteCoreException {
        return new C4BlobKey(computeBlobKey(handle));
    }

    /**
     * Adds the data written to the stream as a finished blob to the store, and returns its key.
     * If you skip this call, the blob will not be added to the store. (You might do this if you
     * were unable to receive all of the data from the network, or if you've called
     * c4stream_computeBlobKey and found that the data does not match the expected digest/key.)
     */
    public void install() throws LiteCoreException {
        install(handle);
    }

    /**
     * Closes a blob write-stream. If c4stream_install was not already called, the temporary file
     * will be deleted without adding the blob to the store.
     */
    public void close() {
        if (handle != 0L) {
            close(handle);
            handle = 0L;
        }
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        close();
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // package methods
    //-------------------------------------------------------------------------
    C4BlobWriteStream(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------
    private native static void write(long writeStream, byte[] bytes) throws LiteCoreException;

    private native static long computeBlobKey(long writeStream) throws LiteCoreException;

    private native static void install(long writeStream) throws LiteCoreException;

    private native static void close(long writeStream);
}
