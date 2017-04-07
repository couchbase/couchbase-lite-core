package com.couchbase.litecore;

/**
 * An open stream for reading data from a blob.
 */
public class C4BlobReadStream {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4BlobReadStream

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    /**
     * Reads from an open stream.
     *
     * @param maxBytesToRead The maximum number of bytes to read to the buffer
     */
    public byte[] read(long maxBytesToRead) throws LiteCoreException {
        return read(handle, maxBytesToRead);
    }

    /**
     * Returns the exact length in bytes of the stream.
     */
    public long getLength() throws LiteCoreException {
        return getLength(handle);
    }

    /**
     * Moves to a random location in the stream; the next c4stream_read call will read from that
     * location.
     */
    public void seek(long position) throws LiteCoreException {
        seek(handle, position);
    }

    /**
     * Closes a read-stream.
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
    C4BlobReadStream(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------
    private native static byte[] read(long readStream, long maxBytesToRead) throws LiteCoreException;

    private native static long getLength(long readStream) throws LiteCoreException;

    private native static void seek(long readStream, long position) throws LiteCoreException;

    private native static void close(long readStream);
}
