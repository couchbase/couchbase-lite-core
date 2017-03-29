package com.couchbase.litecore;

public class C4Query {
    private long handle; // hold pointer to C4Query

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public C4Query(Database db, String expression) throws LiteCoreException {
        handle = init(db._handle, expression);
    }

    public void free() {
        free(handle);
    }

    public String explain() {
        return explain(handle);
    }

    public C4QueryEnumerator run(C4QueryOptions options, String encodedParameters)
            throws LiteCoreException {
        return new C4QueryEnumerator(
                run(handle, options.skip, options.limit, options.rankFullText, encodedParameters));
    }

    public byte[] fullTextMatched(String docID, long seq) throws LiteCoreException {
        return fullTextMatched(handle, docID, seq);
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
     * @param db
     * @param expression
     * @return C4Query*
     * @throws LiteCoreException
     */
    private native static long init(long db, String expression) throws LiteCoreException;

    /**
     * Free C4Query* instance
     *
     * @param c4query (C4Query*)
     */
    private static native void free(long c4query);

    /**
     * @param c4query (C4Query*)
     * @return C4StringResult
     */
    private static native String explain(long c4query);

    /**
     * @param c4query
     * @param skip
     * @param limit
     * @param rankFullText
     * @param encodedParameters
     * @return C4QueryEnumerator*
     * @throws LiteCoreException
     */
    private static native long run(long c4query,
                                   long skip,
                                   long limit,
                                   boolean rankFullText,
                                   String encodedParameters)
            throws LiteCoreException;

    private static native byte[] fullTextMatched(long c4query, String docID, long seq)
            throws LiteCoreException;
}
