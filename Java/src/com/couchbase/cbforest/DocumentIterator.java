package com.couchbase.cbforest;

class DocumentIterator {

    DocumentIterator(long dbHandle) {
        _handle = dbHandle;
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

    
    private native static long init(long dbHandle, long sinceSequence, boolean withBodies)
                throws ForestException;
    private native static long nextDocumentHandle(long handle) throws ForestException;
    private native static void free(long handle);

    private long _handle;
}
