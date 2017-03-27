package com.couchbase.litecore;

/**
 * TODO: Implement  c4db_createIndex() and c4db_deleteIndex(). Maybe in Database.
 */
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

    // TODO: Need to work options
    public C4QueryEnumerator run(long options, String encodedParameters) throws LiteCoreException {
        return new C4QueryEnumerator(run(handle, options, encodedParameters));
    }

    public String fullTextMatched(String docID, long seq) throws LiteCoreException {
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
     * @return
     * @throws LiteCoreException
     */
    private native static long init(long db, String expression) throws LiteCoreException;

    /**
     * Free C4Query* instance
     *
     * @param query (C4Query*)
     */
    private static native void free(long query);

    /**
     * @param query (C4Query*)
     * @return C4StringResult
     */
    private static native String explain(long query);

    /**
     * @param query
     * @param options
     * @param encodedParameters
     * @return C4QueryEnumerator*
     * @throws LiteCoreException
     */
    private static native long run(long query, long options, String encodedParameters) throws LiteCoreException;

    /**
     * @param query
     * @param docID
     * @param seq
     * @return C4StringResult
     * @throws LiteCoreException
     */
    private static native String fullTextMatched(long query, String docID, long seq) throws LiteCoreException;
}
