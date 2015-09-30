package com.couchbase.cbforest;

public class Database {

    // Database-opening flags:
    public static final int Create = 1;
    public static final int ReadOnly = 2;
    public static final int AutoCompact = 4;

    public static final int NoEncryption = 0;
    public static final int AES256Encryption = 1;

    public Database(String path, int flags, int encryptionAlgorithm, byte[] encryptionKey) throws ForestException {
        _handle = _open(path, flags, encryptionAlgorithm, encryptionKey);
    }

    public native void free();

    public native long getDocumentCount();

    public native long getLastSequence();

    public native void beginTransaction() throws ForestException;

    public native void endTransaction(boolean commit) throws ForestException;

    public native boolean isInTransaction();


    public Document getDocument(String docID, boolean mustExist) throws ForestException {
        return new Document(_handle, docID, mustExist);
    }


    public DocumentIterator iterateChanges(long sinceSequence, boolean withBodies) throws ForestException {
        return new DocumentIterator(_iterateChanges(sinceSequence, withBodies));
    }


    protected void finalize() {
        free();
    }

    /** Sets (or clears) a logging callback for CBForest. */
    public native static void setLogger(Logger logger, int level);

    private native long _open(String path, int flags,
                              int encryptionAlgorithm, byte[] encryptionKey) throws ForestException;
    private native long _iterateChanges(long sinceSequence, boolean withBodies) throws ForestException;

    long _handle; // handle to native C4Database*
}
