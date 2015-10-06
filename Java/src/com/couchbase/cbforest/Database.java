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

    public void rawFree(long rawDocHandle) {
        _rawFree(rawDocHandle);
    }

    public void rawPut(String storeName, String key, String meta, byte[] body) throws ForestException {
        _rawPut(_handle, storeName, key, meta, body);
    }

    public long rawGet(String store, String key) throws ForestException {
        return _rawGet(_handle, store, key);
    }

    private native static void _rawFree(long rawDocHandle);
    private native static void _rawPut(long dbHandle, String store, String key, String meta, byte[] body)throws ForestException;
    private native static long _rawGet(long dbHandle, String store, String key)throws ForestException;

    public native static String rawKey(long handle);
    public native static String rawMeta(long handle);
    public native static byte[] rawBody(long handle);


    long _handle; // handle to native C4Database*
}
