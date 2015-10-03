package com.couchbase.cbforest;

class DocumentIterator {
    DocumentIterator(long handle, boolean dummy) {
        _handle = handle;
    }

    DocumentIterator(long dbHandle) throws ForestException {
        _handle = initEnumerateAllDocs(dbHandle, null, null);
    }

    DocumentIterator(long dbHandle, String startDocID, String endDocID) throws ForestException {
        _handle = initEnumerateAllDocs(dbHandle, startDocID, endDocID);
    }

    DocumentIterator(long dbHandle, long sinceSequence) throws ForestException {
        _handle = initEnumerateChanges(dbHandle, sinceSequence);
    }

    // Returns null at end
    public Document nextDocument() throws ForestException {
        long docHandle = nextDocumentHandle(_handle);
        if (docHandle == 0) {
            _handle = 0; // native iterator is already freed
            return null;
        }
        return new Document(docHandle);
    }

    public synchronized void free() { if (_handle != 0) {free(_handle); _handle = 0;} }

    protected void finalize()       { free(); }

    private native long initEnumerateChanges(long dbHandle, long sinceSequence) throws ForestException;
    private native long initEnumerateAllDocs(long dbHandle, String startDocID, String endDocID) throws ForestException;
    private native static long nextDocumentHandle(long handle) throws ForestException;
    private native static void free(long handle);

    private long _handle;
}
