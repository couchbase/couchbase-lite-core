package com.couchbase.litecore.fleece;


public class FLDictIterator {
    private long handle = 0; // hold pointer to FLDictIterator

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLDictIterator() {
        handle = init();
    }

    public void begin(FLDict dict) {
        begin(dict.getHandle(), handle);
    }

    public FLValue getKey() {
        return new FLValue(getKey(handle));
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
     * Create FLDictIterator instance
     *
     * @return long (FLDictIterator *)
     */
    private static native long init();

    /**
     * Initializes a FLDictIterator struct to iterate over a dictionary.
     *
     * @param dict (FLDict)
     * @param itr  (FLDictIterator *)
     */
    private static native void begin(long dict, long itr);

    /**
     * Returns the current key being iterated over.
     *
     * @param itr (FLDictIterator *)
     * @return
     */
    private static native long getKey(final long itr);

    /**
     * Returns the current value being iterated over.
     *
     * @param itr (FLDictIterator *)
     * @return long (FLValue)
     */
    private static native long getValue(final long itr);

    /**
     * Advances the iterator to the next value, or returns false if at the end.
     *
     * @param itr (FLDictIterator *)
     */
    private static native boolean next(long itr);

    /**
     * Free FLDictIterator instance
     *
     * @param itr (FLDictIterator *)
     */
    private static native void free(long itr);
}
