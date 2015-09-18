package com.couchbase.cbforest;

public class Database {

    public Database(String path, boolean readOnly) throws ForestException {
        _handle = _open(path, readOnly);
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

    private native long _open(String path, boolean readOnly) throws ForestException;
    private native long _iterateChanges(long sinceSequence, boolean withBodies) throws ForestException;

    long _handle; // handle to native C4Database*
}
