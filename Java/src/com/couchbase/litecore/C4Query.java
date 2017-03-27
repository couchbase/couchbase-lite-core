package com.couchbase.litecore;

public class C4Query {
    private long handle; // hold pointer to C4Query

    //-------------------------------------------------------------------------
    // internal class
    //-------------------------------------------------------------------------
    public static class C4QueryOptions {
        long skip = 0;
        long limit = Long.MAX_VALUE;
        boolean rankFullText = true;

        public C4QueryOptions() {
        }

        public C4QueryOptions(long skip, long limit, boolean rankFullText) {
            this.skip = skip;
            this.limit = limit;
            this.rankFullText = rankFullText;
        }

        public long getSkip() {
            return skip;
        }

        public void setSkip(long skip) {
            this.skip = skip;
        }

        public long getLimit() {
            return limit;
        }

        public void setLimit(long limit) {
            this.limit = limit;
        }

        public boolean isRankFullText() {
            return rankFullText;
        }

        public void setRankFullText(boolean rankFullText) {
            this.rankFullText = rankFullText;
        }
    }

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

    /**
     * @param c4query
     * @param docID
     * @param seq
     * @return C4StringResult
     * @throws LiteCoreException
     */
    private static native String fullTextMatched(long c4query, String docID, long seq) throws LiteCoreException;
}
