package com.couchbase.cbforest;

public class DocumentIterator {
    DocumentIterator(long handle, boolean dummy) {
        _handle = handle;
    }

    DocumentIterator(long dbHandle,
                     String startDocID,
                     String endDocID,
                     int skip,
                     int iteratorFlags)
            throws ForestException {
        _handle = initEnumerateAllDocs(dbHandle, startDocID, endDocID, skip, iteratorFlags);
    }

    DocumentIterator(long dbHandle, String[] docIDs, int iteratorFlags) throws ForestException {
        _handle = initEnumerateSomeDocs(dbHandle, docIDs, iteratorFlags);
    }

    DocumentIterator(long dbHandle, long sinceSequence, int iteratorFlags) throws ForestException {
        _handle = initEnumerateChanges(dbHandle, sinceSequence, iteratorFlags);
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

    private native long initEnumerateChanges(long dbHandle, long sinceSequence, int optionFlags) throws ForestException;
    private native long initEnumerateAllDocs(long dbHandle,
                                             String startDocID,
                                             String endDocID,
                                             int skip,
                                             int optionFlags)
            throws ForestException;
    private native long initEnumerateSomeDocs(long dbHandle, String[] docIDs, int optionFlags) throws ForestException;
    private native static long nextDocumentHandle(long handle) throws ForestException;
    private native static void free(long handle);

    private long _handle;
}
