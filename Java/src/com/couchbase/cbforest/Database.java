package com.couchbase.cbforest;

public class Database {

    //////// DATABASES:

    // Database-opening flags:
    public static final int Create = 1;
    public static final int ReadOnly = 2;
    public static final int AutoCompact = 4;

    public static final int NoEncryption = 0;
    public static final int AES256Encryption = 1;

    public Database(String path, int flags, int encryptionAlgorithm, byte[] encryptionKey) throws ForestException {
        _handle = _open(path, flags, encryptionAlgorithm, encryptionKey);
    }

    public native void rekey(int encryptionAlgorithm, byte[] encryptionKey) throws ForestException;

    public native void free();

    public native void compact() throws ForestException;

    public native long getDocumentCount();

    public native long getLastSequence();

    public native void beginTransaction() throws ForestException;

    public native void endTransaction(boolean commit) throws ForestException;

    public native boolean isInTransaction();

    public Document getDocument(String docID, boolean mustExist) throws ForestException {
        return new Document(_handle, docID, mustExist);
    }

    public Document getDocumentBySequence(long sequence) throws ForestException {
        return new Document(_handle, sequence);
    }

    public DocumentIterator iterator() throws ForestException {
        return new DocumentIterator(_handle);
    }

    public DocumentIterator iterator(String startDocID, String endDocID) throws ForestException {
        return new DocumentIterator(_handle, startDocID, endDocID);
    }

    public DocumentIterator iterator(String[] docIDs) throws ForestException {
        return new DocumentIterator(_handle, docIDs);
    }

    public DocumentIterator iterator(long sinceSequence) throws ForestException {
        return new DocumentIterator(_handle, sinceSequence);
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


    //////// RAW DOCUMENTS (i.e. info or _local)

    public void rawPut(String store, String key, byte[] meta, byte[] body) throws ForestException {
        _rawPut(_handle, store, key, meta, body);
    }

    // This returns an array of two byte arrays; the first is the meta, the second is the body
    public byte[][] rawGet(String store, String key) throws ForestException {
        return _rawGet(_handle, store, key);
    }

    private native static void _rawPut(long db, String store, String key, byte[] meta, byte[] body) throws ForestException;
    private native static byte[][] _rawGet(long db, String store, String key) throws ForestException;

}
