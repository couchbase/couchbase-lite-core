package com.couchbase.litecore;

public class C4DocEnumerator implements C4Constants {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4DocEnumerator

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------

    C4DocEnumerator(long db, long since, long skip, int flags) throws LiteCoreException {
        handle = enumerateChanges(db, since, skip, flags);
    }

    C4DocEnumerator(long db, String startDocID,
                    String endDocID, long skip, int flags) throws LiteCoreException {
        handle = enumerateAllDocs(db, startDocID, endDocID, skip, flags);
    }

    C4DocEnumerator(long db, String[] docIDs, long skip, int flags) throws LiteCoreException {
        handle = enumerateSomeDocs(db, docIDs, skip, flags);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public void close() {
        close(handle);
    }

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    public boolean next() throws LiteCoreException {
        return next(handle);
    }

    public C4Document getDocument() throws LiteCoreException {
        long doc = getDocument(handle);
        return doc != 0 ? new C4Document(doc) : null;
    }

    public C4Document nextDocument() throws LiteCoreException {
        long doc = nextDocument(handle);
        return doc != 0 ? new C4Document(doc) : null;
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

    static native void close(long e);

    static native void free(long e);

    static native long enumerateChanges(long db,
                                        long since,
                                        long skip,
                                        int flags)
            throws LiteCoreException;

    static native long enumerateAllDocs(long db,
                                        String startDocID,
                                        String endDocID,
                                        long skip,
                                        int flags)
            throws LiteCoreException;

    static native long enumerateSomeDocs(long db,
                                         String[] docIDs,
                                         long skip,
                                         int flags)
            throws LiteCoreException;

    static native boolean next(long e) throws LiteCoreException;

    static native long getDocument(long e) throws LiteCoreException;

    static native void getDocumentInfo(long e, Object[] outIDs, long[] outNumbers);

    static native long nextDocument(long e) throws LiteCoreException;
}
