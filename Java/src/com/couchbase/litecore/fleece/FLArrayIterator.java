package com.couchbase.litecore.fleece;

public class FLArrayIterator {
    private long handle = 0L; // hold pointer to FLArrayIterator

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLArrayIterator() {
        handle = init();
    }

    public void begin(FLArray array) {
        begin(array.getHandle(), handle);
    }

    public FLValue getValue() {
        return new FLValue(getValue(handle));
    }

    public boolean next() {
        return next(handle);
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
    // native methods
    //-------------------------------------------------------------------------

    // TODO: init() and begin() could be combined.

    /**
     * Create FLArrayIterator instance
     *
     * @return long (FLArrayIterator *)
     */
    private static native long init();

    /**
     * Initializes a FLArrayIterator struct to iterate over an array.
     *
     * @param array (FLArray)
     * @param itr   (FLArrayIterator *)
     */
    private static native void begin(long array, long itr);

    /**
     * Returns the current value being iterated over.
     *
     * @param itr (FLArrayIterator *)
     * @return long (FLValue)
     */
    private static native long getValue(final long itr);

    /**
     * Advances the iterator to the next value, or returns false if at the end.
     *
     * @param itr (FLArrayIterator *)
     */
    private static native boolean next(long itr);

    /**
     * Free FLArrayIterator instance
     *
     * @param itr (FLArrayIterator *)
     */
    private static native void free(long itr);
}
